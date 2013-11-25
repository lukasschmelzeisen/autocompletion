/*
 * CompletionServer.cpp
 *
 *  Created on: Nov 14, 2013
 *      Author: Jonas Kunze
 */

#include "CompletionServer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zmq.h>
#include <iosfwd>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <sstream>
#include <thread>
#include <map>

#include "CompletionTrie.h"
#include "SuggestionList.h"
#include "CompletionTrieBuilder.h"
#include "BuilderNode.h"

CompletionServer::CompletionServer() :
		builderThread_(&CompletionServer::builderThread, this), trie(nullptr) {
}

CompletionServer::CompletionServer(CompletionTrie* _trie) :
		builderThread_(&CompletionServer::builderThread, this), trie(_trie) {
}

CompletionServer::~CompletionServer() {
}

static std::string formatSuggestion(const Suggestion sugg) {
	std::stringstream ss;
	ss << "{\"suggestion\":\"" << sugg.suggestion << "\",\"key\":\"" << sugg.URI
			<< "\"";
	if (sugg.image.length() != 0) {
		ss << ",\"image\":\"" << sugg.image << "\"}";
	} else {
		ss << "}";
	}
	return ss.str();
}

std::string CompletionServer::generateResponse(const CompletionTrie* trie,
		char* req, int requestLength) {
	std::string request(req, requestLength);

	std::shared_ptr<SuggestionList> suggestions = trie->getSuggestions(request,
			10);

	std::stringstream jsonStream;
	jsonStream << "{\"suggestionList\": [";
	bool isFirstSuggestion = true;
	for (const Suggestion sugg : suggestions->suggestedWords) {
		if (!isFirstSuggestion) {
			jsonStream << ",";
		} else {
			isFirstSuggestion = false;
		}
		jsonStream << formatSuggestion(sugg);
	}
	jsonStream << "]}";

	std::cout << "Generated response: " << jsonStream.str() << std::endl;

	return jsonStream.str();
}

static int receiveString(void *socket, const unsigned short length,
		char* buffer) {
	int size = zmq_recv(socket, buffer, length, 0);
	if (size == -1)
		return size;
	if (size > length)
		size = length;

	buffer[size] = '\0';
	return size;
}

void CompletionServer::builderThread() {
	void *context = zmq_ctx_new();
	void *socket = zmq_socket(context, ZMQ_PULL);
	std::stringstream address;
	address << BUILDER_ZMQ_PROTO << "://*:" << BUILDER_ZMQ_PORT;
	int rc = zmq_bind(socket, address.str().c_str());
	if (rc != 0) {
		std::cerr << "startBuilderThread: Unable to bind to " << address.str()
				<< std::endl;
		exit(1);
	}

	char dataBuffer[1024];
	std::map<uint64_t, CompletionTrieBuilder*> builders;
	builders.clear();

	while (1) {
		/*
		 * First message: Index
		 */
		uint64_t index;
		while (zmq_recv(socket, &index, sizeof(index), 0) != sizeof(index)) {
			std::cerr
					<< "CompletionServer::builderThread: Unable to receive message"
					<< std::endl;
		}

		/*
		 * Second message: Message type
		 */
		uint8_t msgType;
		int64_t more;
		zmq_recv(socket, &msgType, sizeof(msgType), 0);

		if (msgType == BUILDER_MSG_INSERT) {
			do {
				/*
				 * 3rd message: Term
				 */
				int dataSize = receiveString(socket, sizeof(dataBuffer),
						dataBuffer);
				std::string term(dataBuffer, dataSize);

				/*
				 * 4th message: Score
				 */
				uint32_t score;
				zmq_recv(socket, &score, sizeof(score), 0);

				/*
				 * 5th message: URI
				 */
				dataSize = receiveString(socket, sizeof(dataBuffer),
						dataBuffer);
				std::string URI(dataBuffer, dataSize);

				/*
				 * 6th message: Image
				 * Only read it if there is one more part of the
				 * current multi-part message
				 */
				size_t more_size = sizeof more;
				rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);

				std::string image = "";
				if (rc == 0 && more) {
					zmq_msg_t msg;
					zmq_msg_init(&msg);
					dataSize = zmq_recvmsg(socket, &msg, 0);
//					zmq_msg_recv(&socket, socket, 0);
//					dataSize = receiveString(socket, sizeof(dataBuffer),
//							dataBuffer);
					if (dataSize != -1) {
						image = std::string((char*) zmq_msg_data(&msg),
								dataSize);
						std::cout << "Received image of length " << image.size()
								<< std::endl;
					} else {
						std::cerr
								<< "zmq_recvmsg returned -1 while trying to read image"
								<< std::endl;
					}
				}
				CompletionTrieBuilder* builder = builders[index];
				if (builder == nullptr) {
					std::cerr
							<< "Trying to add term but no CompletionTrieBuilder exists for index "
							<< index << "!" << std::endl;
				} else {
					std::cout << "Adding Term " << term << "\t" << score << "\t"
							<< URI << "\t" << image << std::endl;
					builder->addString(term, score, image, URI);
				}
				rc = zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
			} while (more);

		} else if (msgType == BUILDER_MSG_START_BULK) {
			std::cout << "Received Start Bulk command for index " << index
					<< std::endl;

			if (builders.find(index) != builders.end()) {
				std::cerr << "Starting trie building of index " << index
						<< " but a triBuilder already exists for this index! Will create a new one."
						<< std::endl;
				delete builders[index];
				builders.erase(index);
			}
			CompletionTrieBuilder* builder = new CompletionTrieBuilder();
			builders[index] = builder;
			builder->print();
		} else if (msgType == BUILDER_MSG_STOP_BULK) {
			std::cout << "Received Stop Bulk command for index " << index
					<< std::endl;
			CompletionTrieBuilder* builder = builders[index];
			if (builder == nullptr) {
				std::cerr
						<< "Trying to finish Trie building but no CompletionTrieBuilder exists for index "
						<< index << "!" << std::endl;
			} else {
				builder->print();
				if (this->trie != nullptr) {
					CompletionTrie* tmp = this->trie;
					this->trie = builder->generateCompletionTrie();
					delete tmp;
				} else {
					this->trie = builder->generateCompletionTrie();
				}
				delete builder;
				builders.erase(index);
			}
		}
	}
}

void CompletionServer::start() {
	/*
	 * Connect to pull and push socket of sockjsproxy
	 */
	void *context = zmq_ctx_new();
	void *in_socket = zmq_socket(context, ZMQ_PULL);
	zmq_connect(in_socket, "tcp://localhost:9241");

	void *out_socket = zmq_socket(context, ZMQ_PUSH);
	zmq_connect(out_socket, "tcp://localhost:9242");

	char messageBuffer[13];
	int messageSize;
	char dataBuffer[1500];

	while (1) {
		while ((messageSize = receiveString(in_socket, sizeof(messageBuffer),
				&messageBuffer[0])) <= 0) {
			std::cout << "zmq_recv returned -1" << std::endl;
		}

		uint64_t session_ID;
		zmq_recv(in_socket, &session_ID, sizeof(session_ID), 0);

		int dataSize = receiveString(in_socket, sizeof(dataBuffer), dataBuffer);

		if (strcmp(messageBuffer, "message") == 0) {
			printf("message: %s\n", messageBuffer);
			printf("data: %s\n", dataBuffer);

			zmq_send(out_socket, messageBuffer, messageSize, ZMQ_SNDMORE);
			zmq_send(out_socket, &session_ID, sizeof(session_ID), ZMQ_SNDMORE);

			std::string response = generateResponse(trie, dataBuffer, dataSize);
			zmq_send(out_socket, response.c_str(), response.size(), 0);
		} else if (strcmp(messageBuffer, "connect") == 0) {
			printf("New client: %ld\n", session_ID);
		} else if (strcmp(messageBuffer, "disconnect") == 0) {
			printf("Client disconnected: %ld\n", session_ID);
		}
	}
}
