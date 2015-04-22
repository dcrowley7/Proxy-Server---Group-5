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

#define PORT_NUM "32001"

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

void loadCussWords() {
  std::ifstream myfile("cussWords.txt");

  if (myfile.is_open()) {
	std::string line;

	while (getline (myfile, line)) {
		profanityList.push_back(line);
	}

    myfile.close();
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

void * handleConnection(void * pnewsock) {
	int clientSocket = *(int *)pnewsock;

	char clientRequest[BUFFER_SIZE];

	int clientRequestResult = recv(clientSocket, clientRequest, BUFFER_SIZE, 0);

	if (clientRequestResult <= 0) {
		close(clientSocket);
		return NULL;
	}

	clientRequest[clientRequestResult] = '\0';

	std::string formattedRequest = clientRequest;

	if (currentHostName.length() == 0 || hasHostName(formattedRequest)) {
		currentHostName = getHostName(formattedRequest);
	}

	formattedRequest = removeHostName(currentHostName, formattedRequest);

	if (isBlackListed(currentHostName, blackList)) {
		send403Error(clientSocket);
		close(clientSocket);
		return NULL;
	}

	std::string filename = getFileName(currentHostName, formattedRequest);

	std::cout << "Request  (" << filename << ")" << std::endl;

	int responseLength = getObjectSize(formattedRequest);

	if (responseLength > 0) {
		std::stringstream buffer;

		buffer << formattedRequest;

		int size = responseLength - formattedRequest.length();

		while (size > 0) {
			clientRequestResult = recv(clientSocket, clientRequest, BUFFER_SIZE, 0);

			if (clientRequestResult > 0) {
				clientRequest[clientRequestResult] = '\0';
				buffer << clientRequest;
			} else {
				close(clientSocket);
				return NULL;
			}

			size -= clientRequestResult;
		}

		formattedRequest = buffer.str();
	} else {
		std::cout << "Chunked request encountered." << std::endl;
		exit(0);
	}

	formattedRequest = stripHeader(formattedRequest, currentHostName);

	if(!fileCached(filename, clientSocket)) {
		int webServerSocket = contactServer(currentHostName);

		int serverSendResult = send(webServerSocket, formattedRequest.c_str(), formattedRequest.length(), 0);

		if (serverSendResult == -1) {
			std::cout << "Send error. (Server)" << std::endl;
			close(webServerSocket);
			close(clientSocket);
			return NULL;
		}

		char serverResponse[BUFFER_SIZE];

		const int webServerResponse = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

		if (webServerResponse < 0) {
			close(webServerSocket);
			close(clientSocket);
			return NULL;
		}

		responseLength = getObjectSize(serverResponse);

		if (responseLength == 0) {
			while (!hasEnded(serverResponse)) {
				const int webServerResponseResult = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

				if (webServerResponseResult < 0) {
					break;
				}
			}
		} else {
			int size = responseLength - webServerResponse;

			while (size > 0) {
				const int webServerResponseResult = receiveSend(webServerSocket, clientSocket, serverResponse, filename, profanityList);

				if (webServerResponseResult < 0) {
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

int main(int argc, char *argv[]) {;
	blackList.push_back("youtube.com");
	blackList.push_back("facebook.com");
	blackList.push_back("hulu.com");
	blackList.push_back("virus.com");

	loadCussWords();

	struct addrinfo hints, *res;

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
