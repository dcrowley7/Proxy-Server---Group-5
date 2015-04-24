#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <iostream>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <string.h>
#include <vector>
#include <cstring>

#define PORT_NUM "8080"

const int BUFFER_SIZE = 4096;

std::string currentHostName = "";

std::vector<std::string> blackList,
                         profanityList;

void send403Error(int);
bool hasEnded(std::string);
bool hasHostName(std::string);
bool fileCached(std::string, int);
bool isBlackListed(std::string, std::vector<std::string>);
int getObjectSize(std::string);
int receiveSend(int, int, char *, std::string, std::vector<std::string>);
std::string stripHeader(std::string, std::string);
std::string getFileName(std::string, std::string);
std::string getHostName(std::string);
std::string removeHostName(std::string, std::string);

/* Loads cuss words from text file for profanity filter */
void loadCussWords() {
  std::ifstream cussWordsFile("cussWords.txt");

  if (cussWordsFile.is_open()) {
	std::string line;

	while (getline(cussWordsFile, line)) {
		profanityList.push_back(line);
	}

	cussWordsFile.close();
  } else {
	std::cout << "Unable to load cuss word filter." << std::endl
	          << "Please make sure 'cussWords.txt' is in the same directory as the proxy server." << std::endl;
  }
}

void * get_in_addr(struct sockaddr * sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in *)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

/* Connects the proxy to the web server */
int contactServer(std::string hostname) {
	int sockfd;

    struct addrinfo hints, *servinfo, *p;

    int rv;

    char s[INET6_ADDRSTRLEN];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(hostname.c_str(), "80", &hints, &servinfo)) != 0) {
    	std::cout << hostname << std::endl;
        perror("Getaddrinfo call failed.");
    }

    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
    		perror("Could not connect.");
            continue;
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
    		perror("Could not connect.");
            continue;
        }

        break;
    }

    if (p == NULL) {
		perror("Could not connect.");
    }

    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), s, sizeof(s));

    freeaddrinfo(servinfo);

    return sockfd;
}

/* Multithreaded function that handles the request and response for a single object */
void * handleConnection(void * pnewsock) {
	int clientSocket = *(int *)pnewsock;

	char clientRequest[BUFFER_SIZE];

	// Receive client request
	int clientRequestResult = recv(clientSocket, clientRequest, BUFFER_SIZE, 0);

	if (clientRequestResult <= 0) {
		std::cout << "Read error when reading first request from client: " << clientRequestResult << std::endl;
		close(clientSocket);
		return NULL;
	}

	// Cut off excess data
	clientRequest[clientRequestResult] = '\0';

	std::string formattedRequest = clientRequest;

	// Grab hostname if not already connected to the web server
	if (currentHostName.length() == 0 || hasHostName(formattedRequest)) {
		std::string hostName = getHostName(formattedRequest);

		if (hostName.length() != 0) {
			currentHostName = hostName;
		} else {
			std::cout << "Could not extract hostname from the request" << std::endl;
			close(clientSocket);
			return NULL;
		}

		// Remove hostname from request for simplicity's sake
		formattedRequest = removeHostName(currentHostName, formattedRequest);

		if (formattedRequest.length() == 0) {
			std::cout << "Could not remove hostname from request." << std::endl;
			close(clientSocket);
			return NULL;
		}
	}

	// Check if site is blacklisted
	if (isBlackListed(currentHostName, blackList)) {

		// Send 403 - Forbidden page if blacklisted
		send403Error(clientSocket);
		close(clientSocket);
		return NULL;
	}

	// Create the filename (including directory structure) for locating an object
	// 	      if it is cached
	std::string filename = getFileName(currentHostName, formattedRequest);

	if (filename.length() == 0) {
		std::cout << "Could not parse request for object name." << std::endl;
		close(clientSocket);
		return NULL;
	}

	// Log request
	std::cout << "Request  (" << filename << ")" << std::endl;

	// Get object size (content-length + header or actual message size)
	int responseLength = getObjectSize(formattedRequest);

	// Get entire request (if larger than buffer)
	if (responseLength > 0) { // For non-chunked requests
		std::stringstream buffer;

		buffer << formattedRequest;

		int size = responseLength - formattedRequest.length();

		while (size > 0) {
			clientRequestResult = recv(clientSocket, clientRequest, BUFFER_SIZE, 0);

			if (clientRequestResult > 0) {
				clientRequest[clientRequestResult] = '\0';
				buffer << clientRequest;
			} else {
				std::cout << "Read error when getting the rest of the request from the client. (Non-Chunked)" << std::endl;
				close(clientSocket);
				return NULL;
			}

			size -= clientRequestResult;
		}

		formattedRequest = buffer.str();
	} else { // For chunked requests
		std::stringstream buffer;

		buffer << formattedRequest;

		int size = responseLength - formattedRequest.length();

		while (!hasEnded(clientRequest)) {
			clientRequestResult = recv(clientSocket, clientRequest, BUFFER_SIZE, 0);

			if (clientRequestResult > 0) {
				clientRequest[clientRequestResult] = '\0';
				buffer << clientRequest;
			} else {
				std::cout << "Read error when getting the rest of the request from the client. (Chunked)" << std::endl;
				close(clientSocket);
				return NULL;
			}
		}

		formattedRequest = buffer.str();
	}

	// Remove web browsers headers and add our own to the request
	formattedRequest = stripHeader(formattedRequest, currentHostName);

	// Retrieve object from the webserver if the object is not already cached
	if(!fileCached(filename, clientSocket)) {

		// Connect to web server
		int webServerSocket = contactServer(currentHostName);

		// Send request to webserver
		int serverSendResult = send(webServerSocket, formattedRequest.c_str(), formattedRequest.length(), 0);

		if (serverSendResult == -1) {
			std::cout << "Send error when sending first chunk of request to web server." << std::endl;
			close(webServerSocket);
			close(clientSocket);
			return NULL;
		}

		char serverResponse[BUFFER_SIZE];

		// Receive data from webserver and send directly to client
		const int webServerResponse = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

		if (webServerResponse < 0) {
			std::cout << "Read error when receiving first chunk of response from web server." << std::endl;
			close(webServerSocket);
			close(clientSocket);
			return NULL;
		}

		// Get size of response
		responseLength = getObjectSize(serverResponse);

		// Grab the entire response if larger than buffer
		if (responseLength == 0) { // Chunked responses

			// Loop until all bytes have been received and sent
			while (!hasEnded(serverResponse)) {

				// Receive chunk from server and send immediately to client
				const int webServerResponseResult = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

				if (webServerResponseResult < 0) {
					std::cout << "Read error when receiving additional chunks of response from web server. (Chunked Response)" << std::endl;
					break;
				}
			}
		} else { // Non-chunked responses
			int size = responseLength - webServerResponse;

			// Loop until all bytes have been received and sent
			while (size > 0) {

				// Receive chunk from server and send immediately to client
				const int webServerResponseResult = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

				if (webServerResponseResult < 0) {
					std::cout << "Read error when receiving additional chunks of response from web server. (Non-Chunked Response)" << std::endl;
					break;
				}

				size -= webServerResponseResult;
			}
		}

		close(webServerSocket);
	}

	close(clientSocket);
	return NULL;
}

int main(int argc, char *argv[]) {
	std::cout << "Starting proxy server..." << std::endl;

	// Add blacklisted sites
	blackList.push_back("youtube.com");
	blackList.push_back("facebook.com");
	blackList.push_back("hulu.com");
	blackList.push_back("virus.com");

	// Load cuss filter from file
	loadCussWords();

	struct addrinfo hints, *res;

	/* Set up socket connection for incoming client requests */

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(NULL, PORT_NUM, &hints, &res) != 0) {
		perror("Initialization failed.");
		return 1;
	}

	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

	if (sock == -1) {
		perror("Initialization failed.");
		return 1;
	}

	int reuseaddr = 1;

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) == -1) {
		perror("Initialization failed.");
		return 1;
	}

	if (bind(sock, res->ai_addr, res->ai_addrlen) == -1) {
		perror("Initialization failed.");
		return 1;
	}

	freeaddrinfo(res);

	if (listen(sock, 10) == -1) {
		perror("Initialization failed.");
		return 1;
	}

    pthread_t thread;

	// Loop until server is closed
	while (true) {
		struct sockaddr_in address;

		size_t size = sizeof(address);

		// Accept a connection from a new client
		int newConnection = accept(sock, (struct sockaddr *)&address, (unsigned int *)&size);

		if (newConnection >= 0) {

			// Handle connection and give the new client their own thread
			if (pthread_create(&thread, NULL, handleConnection, &newConnection) != 0) {
				perror("Could not create thread for the client.");
			}
		} else {
			perror("Could not accept connection.");
		}
	}

    close(sock);

    return 0;
}
