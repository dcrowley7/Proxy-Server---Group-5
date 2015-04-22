#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

const int BUFFER_SIZE = 4096;

bool isBlackListed(std::string hostname, std::vector<std::string> blackList) {
	for (unsigned int count = 0; count < blackList.size(); count++) {
		if(hostname.find(blackList[count]) != std::string::npos) {
			return true;
		}
	}

	return false;
}

void saveToFile(std::string filename, char * data, int size) {
	int filenameIndex = filename.find_last_of("/");

	std::string command = "mkdir -p Data/" + filename.substr(0, filenameIndex);

	filename = "Data/" + filename;

	system(command.c_str());

	std::ofstream myfile;

	myfile.open(filename.c_str(), std::ios_base::app);
	myfile.write(data, size);
	myfile.close();
}

int sendAll(int s, char *buf, int *len) {
    int total = 0;        // how many bytes we've sent
    int bytesleft = *len; // how many we have left to send
    int n;

    while(total < *len) {
        n = send(s, buf+total, bytesleft, 0);
        if (n == -1) { break; }
        total += n;
        bytesleft -= n;
    }

    *len = total; // return number actually sent here

    return n==-1?-1:0; // return -1 on failure, 0 on success
}

std::string getHostName(std::string request) {
    unsigned int isFromURL = request.find("GET /");

	if (isFromURL != std::string::npos) {
		request = request.substr(isFromURL + 5);
		request = request.substr(0, request.find("HTTP/"));

		unsigned int end = request.find("/");

		if (end == std::string::npos) {
			end = request.find(" ");
		}

		request = request.substr(0, end);
	} else {
		// Error handling
		// Unknown host
	}

	return request;
}

int getObjectSize(std::string response) {
	int objectSize = 0;

	std::string headerName = "Content-Length: ";

	std::size_t headerLength       = response.find("\r\n\r\n") + 4,
			    contentHeaderIndex = response.find(headerName);

	if (std::string::npos == contentHeaderIndex) {
		if (std::string::npos == response.find("Transfer-Encoding: chunked")) {
			objectSize = response.length();
		}
	} else {
		contentHeaderIndex += headerName.length();

		std::size_t contentHeaderEndIndex = response.find("\r", contentHeaderIndex);

		int contentLength = atoi(response.substr(contentHeaderIndex, contentHeaderEndIndex - contentHeaderIndex).c_str());

		objectSize = contentLength + headerLength;
	}

	return objectSize;
}

bool hasEnded(std::string response) {
	std::size_t found = response.find("0\r\n\r\n");

	if (std::string::npos == found) {
		return false;
	} else {
		return true;
	}
}

std::string getFileName(std::string hostname, std::string request) {
	unsigned int start = request.find(" /") + 1,
		         end   = request.find(" HTTP/");

	if (start != std::string::npos && end != std::string::npos) {
		request = request.substr(start, end - start);

		if (request.find_last_of("/") == request.length() - 1) {
			request += "index.html";
		} else if (request.find("/") == std::string::npos) {
			request += "/index.html";
		}
	} else {
		// Error
		// File name parsing
	}

	return (hostname + request);
}

std::string stripHeader(std::string stripMe, std::string hostname) {
	std::cout << stripMe << std::endl;

	std::string newHeader   = stripMe.substr(0, stripMe.find("\r\n") + 2),
			    requestBody = stripMe.substr(stripMe.find("\r\n\r\n"));

	std::vector<std::string> headers;
	headers.push_back("User-Agent");
	headers.push_back("Content-Length");

	for (unsigned int count = 0; count < headers.size(); count++) {

		std::size_t startIndex = stripMe.find(headers[count]);

		std::size_t endIndex = stripMe.find("\r\n", startIndex);

		if (startIndex != std::string::npos && endIndex != std::string::npos) {
			newHeader += stripMe.substr(startIndex, endIndex - startIndex + 2);
		}
	}

	newHeader += "Host: "    + hostname + "\r\n";
	newHeader += "Referer: " + hostname + "\r\n";
	newHeader += "Connection: close";

	return (newHeader + requestBody);
}

bool fileCached(std::string objectName, int clientSocket) {
	objectName = "Data/" + objectName;

	std::ifstream file(objectName.c_str(), std::ifstream::binary);

	bool cacheHit = false;

	if (file.good()) {
		// get pointer to associated buffer object
		std::filebuf* pbuf = file.rdbuf();

		// get file size using buffer's members
		std::size_t size = pbuf->pubseekoff(0, file.end, file.in);
		pbuf->pubseekpos(0, file.in);

		// allocate memory to contain file data
		char * buffer = new char[size];

		int length = size;

		// get file data
		pbuf->sgetn(buffer, size);

		file.close();

		int clientSendResult = sendAll(clientSocket, buffer, &length);

		delete[] buffer;

		if (clientSendResult == -1) {
			std::cout << "Send error." << std::endl;
			close(clientSocket);
		}

		std::cout << "Cache hit on: " << objectName << std::endl;;

		cacheHit = true;
	}

	file.close();

	return cacheHit;
}

void send404Error(int clientSocket) {
	std::string response  = "HTTP/1.1 404 Not Found\r\nContent-type: text/html\r\nContent-length: 135\r\n\r\n";
	            response += "<html><head><title>Not Found</title></head><body>Sorry, the object you requested was not found.</body><html>";

	send(clientSocket, response.c_str(), response.length(), 0);

	std::cout << "404 sent!" << std::endl;
}

void send403Error(int clientSocket) {
	std::string response  = "HTTP/1.1 403 Forbidden\r\nContent-type: text/html\r\nContent-length: 109\r\n\r\n";
	            response += "<html><head><title>Forbidden</title></head><body>Sorry, the object you requested is blacklisted.</body><html>";

	send(clientSocket, response.c_str(), response.length(), 0);

	std::cout << "Blacklisted error sent!" << std::endl;
}

std::string removeHostName(std::string hostname, std::string request) {
	unsigned int beginning = request.substr(0, request.find("\r\n")).find(hostname);

	if (beginning != std::string::npos) {
		request = request.substr(0, beginning) + request.substr(beginning + hostname.length());

		unsigned int index = request.find("//");

		if (index != std::string::npos) {
			request = request.substr(0, index) + request.substr(index + 1);
		}
	}

    return request;
}

bool hasHostName(std::string request) {
	int beginning = request.find(" /"),
	    ending    = request.find(" HTTP/");

	request = request.substr(beginning + 2, ending - beginning + 2);

	std::vector<std::string> identifiers;
	identifiers.push_back(".com");
	identifiers.push_back(".net");
	identifiers.push_back(".org");
	identifiers.push_back("www.");
	identifiers.push_back(".edu");

	for (unsigned int count = 0; count < identifiers.size(); count++) {
		if (request.find(identifiers[count]) != std::string::npos) {
			return true;
		}
	}

	return false;
}

std::string removeProfanity(std::string response, std::vector<std::string> profanityList) {
	for (unsigned int count = 0; count < profanityList.size(); count++) {
		for (int count2 = 0; count2 < 5; count2++) {
			unsigned int index = response.find(" " + profanityList[count]);

			if(index != std::string::npos) {
				std::string replacement = "";

				for (unsigned int letters = 0; letters < profanityList[count].length(); letters++) {
					replacement += "*";
				}

				response = response.replace(index + 1, profanityList[count].length(), replacement);
			} else {
				break;
			}
		}
	}

	return response;
}

int receiveSend(int webServerSocket, int clientSocket, char * serverResponse, std::string filename, std::vector<std::string> profanityList) {
	const int webServerResponseResult = recv(webServerSocket, serverResponse, BUFFER_SIZE, 0);

	if (webServerResponseResult <= 0) {
		send404Error(clientSocket);

		return -1;
	}

	if (filename.find(".htm") != std::string::npos) {
		std::string cleanResponse = removeProfanity(serverResponse, profanityList);
		strcpy(serverResponse, cleanResponse.c_str());
	}

	saveToFile(filename, serverResponse, webServerResponseResult);

	int clientSendResult = send(clientSocket, serverResponse, webServerResponseResult, 0);

	if (clientSendResult == -1) {
		std::cout << "Send error." << std::endl;

		return -1;
	}

	return webServerResponseResult;
}
