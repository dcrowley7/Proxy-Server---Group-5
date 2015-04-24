#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

const int BUFFER_SIZE = 4096;

/* Check if URL provided is blacklisted */
bool isBlackListed(std::string hostname, std::vector<std::string> blackList) {
	for ( int count = 0; count < blackList.size(); count++) {
		if(hostname.find(blackList[count]) != std::string::npos) {
			return true;
		}
	}

	return false;
}

/* Cache web object */
void saveToFile(std::string filename, char * data, int size) {
	int filenameIndex = filename.find_last_of("/");

	// Create command to create directory structure
	std::string command = "mkdir -p Data/" + filename.substr(0, filenameIndex);

	filename = "Data/" + filename;

	// Create directory structure for the object
	system(command.c_str());

	std::ofstream webObject;

	// Cache object
	if (webObject.good()) {
		webObject.open(filename.c_str(), std::ios_base::app);
		webObject.write(data, size);
	} else {
		std::cout << "Could not cache object: " << filename << std::endl;
	}

	webObject.close();
}

/* For sending messages larger than the buffer */
int sendAll(int sock, char * buffer, int * length) {
    int total = 0,
    	bytesleft = * length,
        response;

    // Loop until all bytes have been sent
    while(total < * length) {
    	response = send(sock, buffer + total, bytesleft, 0);
        if (response == -1) { break; }
        total += response;
        bytesleft -= response;
    }

    // Set the amount of data sent
    * length = total;

    // Return -1 on failure
    return (response == -1) ? -1 : 0;
}

/* Extracts hostname from a request */
std::string getHostName(std::string request) {
    int isFromURL = request.find("GET /");

    // First request should always be a get
	if (isFromURL != std::string::npos) {
		request = request.substr(isFromURL + 5);
		request = request.substr(0, request.find("HTTP/"));

		 int end = request.find("/");

		if (end == std::string::npos) {
			end = request.find(" ");
		}

		// Load bare hostname (ie www.google.com)
		request = request.substr(0, end);
	// If request is not a get
	} else {
		request = "";
	}

	return request;
}

/* Gets the size of the web object. Returns 0 if the object is chunked */
int getObjectSize(std::string response) {
	int objectSize = 0;

	std::string headerName = "Content-Length: ";

	std::size_t headerLength       = response.find("\r\n\r\n") + 4, // Header ending
			    contentHeaderIndex = response.find(headerName);     // Index of content-length header

	// If content-length not found
	if (std::string::npos == contentHeaderIndex) {
		// If no content-length or chunked encoding
		if (std::string::npos == response.find("Transfer-Encoding: chunked")) {
			objectSize = response.length();
		}
	// If content-length found
	} else {
		contentHeaderIndex += headerName.length();

		std::size_t contentHeaderEndIndex = response.find("\r", contentHeaderIndex);

		int contentLength = atoi(response.substr(contentHeaderIndex, contentHeaderEndIndex - contentHeaderIndex).c_str());

		// Header size + body size
		objectSize = contentLength + headerLength;
	}

	// Return 0 if its a chunked request/response
	// Return header size + content-length if 'content-length' found
	// Return message size otherwise
	return objectSize;
}

/* Checks if we are at the end of a chunked response */
bool hasEnded(std::string response) {
	std::size_t found = response.find("0\r\n\r\n");

	// Return false if chunked terminator not found
	if (std::string::npos == found) {
		return false;
	} else {
		return true;
	}
}

/* Extracts/creates name of a web object plus the directory it should be saved in */
std::string getFileName(std::string hostname, std::string request) {
	int start = request.find(" /") + 1,
		         end   = request.find(" HTTP/");

	// Get hostname + directory + filename
	if (start != std::string::npos && end != std::string::npos) {
		request = request.substr(start, end - start);

		// If we requested say www.google.com, we need an object name in order to cache it
		//    To do this, we tack on a filename of 'index.html'
		if (request.find_last_of("/") == request.length() - 1) {
			request += "index.html";
		} else if (request.find("/") == std::string::npos) {
			request += "/index.html";
		}
	// Not found?
	} else {
		hostname = "";
		request  = "";
	}

	// Return host + file name (ie www.google.com/index.html)
	return (hostname + request);
}

/* Removes web browser's headers and adds our own */
std::string stripHeader(std::string stripMe, std::string hostname) {
	std::string newHeader   = stripMe.substr(0, stripMe.find("\r\n") + 2), // Grab first line (usually GET /blahblah.html HTTP/1.1)
			    requestBody = stripMe.substr(stripMe.find("\r\n\r\n"));    // Get index of header ending

	// Load headers we want to keep from the original request
	std::vector<std::string> headers;
	headers.push_back("User-Agent");
	headers.push_back("Content-Length");

	// Loop through each header we want to keep and at it to the new header
	for ( int count = 0; count < headers.size(); count++) {

		std::size_t startIndex = stripMe.find(headers[count]);

		std::size_t endIndex = stripMe.find("\r\n", startIndex);

		if (startIndex != std::string::npos && endIndex != std::string::npos) {
			newHeader += stripMe.substr(startIndex, endIndex - startIndex + 2);
		}
	}

	// Add our own headers
	newHeader += "Host: "    + hostname + "\r\n";
	newHeader += "Referer: " + hostname + "\r\n";
	newHeader += "Connection: close";

	// Return header + body
	return (newHeader + requestBody);
}

/* Checks if a file is cached and then retrieves it from the cache */
bool fileCached(std::string objectName, int clientSocket) {
	objectName = "Data/" + objectName;

	// Open cached object
	std::ifstream file(objectName.c_str(), std::ifstream::binary);

	bool cacheHit = false;

	// If we found it, open it
	if (file.good()) {
		std::filebuf* pbuf = file.rdbuf();

		// Get file size
		std::size_t size = pbuf->pubseekoff(0, file.end, file.in);
		pbuf->pubseekpos(0, file.in);

		// Allocate memory to contain the file data
		char * buffer = new char[size];

		int length = size;

		// Get file data
		pbuf->sgetn(buffer, size);

		file.close();

		// Send object to client
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

	// Return true if object found, false otherwise
	return cacheHit;
}

/* Sends a 404 error message back to the browser */
void send404Error(int clientSocket) {
	std::string response  = "HTTP/1.1 404 Not Found\r\nContent-type: text/html\r\nContent-length: 135\r\n\r\n";
	            response += "<html><head><title>Not Found</title></head><body>Sorry, the object you requested was not found.</body><html>";

	send(clientSocket, response.c_str(), response.length(), 0);

	std::cout << "404 sent!" << std::endl;
}

/* Sends a 403 - Forbidden message back to the browser */
void send403Error(int clientSocket) {
	std::string response  = "HTTP/1.1 403 Forbidden\r\nContent-type: text/html\r\nContent-length: 109\r\n\r\n";
	            response += "<html><head><title>Forbidden</title></head><body>Sorry, the object you requested is blacklisted.</body><html>";

	send(clientSocket, response.c_str(), response.length(), 0);

	std::cout << "Blacklisted error sent!" << std::endl;
}

/* Removes the hostname from the http request message */
std::string removeHostName(std::string hostname, std::string request) {
	int beginning = request.substr(0, request.find("\r\n")).find(hostname); // Grab first line

	if (beginning != std::string::npos) {
		// Find hostname and extract it
		request = request.substr(0, beginning) + request.substr(beginning + hostname.length());

		// Sometimes '//' is left over after the hostname is extracted
		 int index = request.find("//");

		// Remove one of the slashes if found
		if (index != std::string::npos) {
			request = request.substr(0, index) + request.substr(index + 1);
		}
	} else {
		request = "";
	}

	// Return request without a hostname (ie GET /index.html HTTP/1.1 ... <BODY>)
    return request;
}

/* Checks if the request has a hostname */
bool hasHostName(std::string request) {
	int beginning = request.find(" /"),
	    ending    = request.find(" HTTP/");

	// Cut out hostname section from first line
	request = request.substr(beginning + 2, ending - beginning + 2);

	// Extensions to filter by
	std::vector<std::string> identifiers;
	identifiers.push_back(".com");
	identifiers.push_back(".net");
	identifiers.push_back(".org");
	identifiers.push_back("www.");
	identifiers.push_back(".edu");

	// Return true if a hostname extension is found
	for ( int count = 0; count < identifiers.size(); count++) {
		if (request.find(identifiers[count]) != std::string::npos) {
			return true;
		}
	}

	return false;
}

/* Removes cuss words from an html file */
std::string removeProfanity(std::string response, std::vector<std::string> profanityList) {
	// Loop through each cuss word in our list
	for (int count = 0; count < profanityList.size(); count++) {

		// Find all instances of the cuss word in the object
		while(true) {
			int index = response.find(" " + profanityList[count]);

			if (index == std::string::npos) {
				index = response.find("\n" + profanityList[count]);
			}

			// If found
			if(index != std::string::npos) {
				std::string replacement = "";

				for ( int letters = 0; letters < profanityList[count].length(); letters++) {
					replacement += "*";
				}

				// Replace cuss word with asterisks
				response = response.replace(index + 1, profanityList[count].length(), replacement);
			} else {
				break;
			}
		}
	}

	// Return filtered request
	return response;
}

/* Receives data from the server and immediately sends it to the client */
int receiveSend(int webServerSocket, int clientSocket, char * serverResponse, std::string filename, std::vector<std::string> profanityList) {

	// Receive response from web server
	const int webServerResponseResult = recv(webServerSocket, serverResponse, BUFFER_SIZE, 0);

	if (webServerResponseResult <= 0) {
		send404Error(clientSocket);

		return -1;
	}

	// Filter bad words if it is an html/htm file
	if (filename.find(".htm") != std::string::npos || filename.substr((filename.find_last_of("/"))).find(".") == std::string::npos) {
		std::string cleanResponse = removeProfanity(serverResponse, profanityList);
		strcpy(serverResponse, cleanResponse.c_str());
	}

	// Cache object
	saveToFile(filename, serverResponse, webServerResponseResult);

	// Send response to client
	int clientSendResult = send(clientSocket, serverResponse, webServerResponseResult, 0);

	if (clientSendResult == -1) {
		std::cout << "Send error." << std::endl;

		return -1;
	}

	// Return the number of bytes received from server
	return webServerResponseResult;
}
