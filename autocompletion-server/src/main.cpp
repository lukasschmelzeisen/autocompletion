//============================================================================
// Name        : main.cpp
// Author      : Jonas Kunze
// Version     :
// Copyright   : GPLv3
// Description : Main method of the autocompletion service
//============================================================================

#include <iostream>
#include <memory>
#include <string>

#include <autocompletion/CompletionTrie.h>
#include <autocompletion/CompletionTrieBuilder.h>
#include <autocompletion/SuggestionList.h>
#include <autocompletion/utils/Utils.h>

#include "options/Options.h"
#include "CompletionServer.h"

using namespace std;

void interactiveThread(const CompletionTrie* trie) {
	do {
		std::string str;
		std::cout << "Please enter search string: ";
		std::cin >> str;

		if (str == "\\q") {
			return;
		}

		long start = Utils::getCurrentMicroSeconds();
		std::shared_ptr<SuggestionList> suggestions = trie->getSuggestions(str,
				12);
		long time = Utils::getCurrentMicroSeconds() - start;
		std::cout << time << " us for finding suggestions" << std::endl;

		for (Suggestion sugg : suggestions->suggestedWords) {
			std::cout << sugg.suggestion << "\t" << sugg.relativeScore << "\t"
					<< sugg.additionalData << std::endl;
		}
	} while (true);
}

int main(int argc, char* argv[]) {
	Options::Initialize(argc, argv);

	if (Options::Isset(OPTION_LOAD_FILE)) {
		CompletionTrie* trie = CompletionTrieBuilder::buildFromFile(
				Options::GetString(OPTION_LOAD_FILE), true, false);

		if (trie->getMemoryConsumption() < 100) {
			if (Options::VERBOSE)
				trie->print();
		}

		//	PerformanceTest::runTest(trie);
		interactiveThread(trie);

		std::thread t(&interactiveThread, trie);
	}

	CompletionServer server;
	server.run();

	return 0;
}
