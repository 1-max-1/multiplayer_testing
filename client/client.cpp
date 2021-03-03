#include <winsock2.h>
#include <Ws2tcpip.h>
#include <iostream>
#include <vector>
#include <string>

#include <SDL.h>
#include <SDL_image.h>

#define MAX_CLIENTS 2
#define CLIENTS_PER_GAME 2
#if MAX_CLIENTS % CLIENTS_PER_GAME != 0
#error MAX_CLIENTS cannot fit evenly into CLIENTS_PER_GAME. Please adjust values.
#endif

using namespace std;

class Game {
public:
	Game() {
		window = NULL;
		renderer = NULL;
		playerTexture = NULL;
		clickToPlayTexture = nullptr;

		connectedPlayers = 0;
		ID = 0;
		connectedToServer = false;
		onMenuScreen = true;

		timeOfLastServerMsg = 0;
		timeOfPrevFrame = 0;

		clientMsgNum = 0;
		sock = NULL;
		serverAddress = {0};
		
		frameCount = 0;
	}

	~Game() {
		WSACleanup();

		SDL_DestroyTexture(playerTexture);
		playerTexture = NULL;
		SDL_DestroyTexture(clickToPlayTexture);
		clickToPlayTexture = nullptr;

		SDL_DestroyRenderer(renderer);
		renderer = NULL;
		SDL_DestroyWindow(window);
		window = NULL;

		cout << "Average FPS: " << frameCount / (SDL_GetTicks() / 1000) << endl;

		IMG_Quit();
		SDL_Quit();

		char input;
		cin >> input;
	}

	void initSDL() {
		if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO) < 0) {
			cout << "Couldn't initialize sdl: " << SDL_GetError() << endl;
		}

		if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) != IMG_INIT_PNG) {
			cout << "Couldn't initialize SDL_image: " << IMG_GetError() << endl;
		}

		window = SDL_CreateWindow("Multiplayer testing", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 500, 500, SDL_WINDOW_SHOWN);
		if (window == NULL) {
			cout << "Couldn't create window: " << SDL_GetError() << endl;
		}

		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		if (renderer == NULL) {
			cout << "Couldn't create renderer: " << SDL_GetError() << endl;
		}

		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	}

	void initWinsock() {
		WSAData winsockData;
		if (WSAStartup(0x202, &winsockData)) {
			cout << "Failed to start winsock: " << WSAGetLastError();
		}

		sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == INVALID_SOCKET) {
			cout << "Couldn't create the UDP socket: " << WSAGetLastError();
		}

		// Put socket in non-blocking mode
		u_long argp = 1;
		cout << ioctlsocket(sock, FIONBIO, &argp) << endl;

		serverAddress.sin_family = AF_INET;
		serverAddress.sin_port = htons(5555);
		inet_pton(AF_INET, "192.168.20.10", &serverAddress.sin_addr.S_un.S_addr);
	}

	void load() {
		SDL_Surface* surface = IMG_Load("player.png");
		if (surface == NULL)
			cout << "IMG_Load() failed for some reason: " << IMG_GetError() << endl;
		playerTexture = SDL_CreateTextureFromSurface(renderer, surface);
		if (playerTexture == NULL)
			cout << "Failed to create the texture for some reason: " << SDL_GetError() << endl;

		surface = IMG_Load("click_to_play.png");
		if (surface == NULL)
			cout << "IMG_Load() failed for some reason: " << IMG_GetError() << endl;
		clickToPlayTexture = SDL_CreateTextureFromSurface(renderer, surface);
		if (clickToPlayTexture == NULL)
			cout << "Failed to create the texture for some reason: " << SDL_GetError() << endl;

		SDL_FreeSurface(surface);
	}

	bool doFrame() {
		// Handle window events ///////////////
		SDL_Event eventStructure;
		while (SDL_PollEvent(&eventStructure) != 0) {
			if (eventStructure.type == SDL_QUIT)
				return true;
			if (eventStructure.type == SDL_MOUSEBUTTONDOWN && connectedToServer == false) {
				// Send a join message to the server
				buffer[0] = (byte)clientDataTypes::join;
				if (sendto(sock, (char*)buffer, 1, 0, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
					cout << "Join message failed to send\n";
				else {
					onMenuScreen = false;
					timeOfLastServerMsg = SDL_GetTicks();
				}
			}
		}
		///////////////////////////////////////

		if (onMenuScreen) {
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, clickToPlayTexture, NULL, NULL);
			SDL_RenderPresent(renderer);
			return false;
		}

		SOCKADDR_IN recvAddress;
		int recvAddressSize = sizeof(recvAddress);

		//!!!!!!!!!!!!!! VERY IMPORTANT NOTE !!!!!!!!!!!!!//
		// Need a while loop to recv ALL packets from the server. This is very important. If we don't do this, some packets won't be received this tick.
		// This creates the appearance of high ping, even in a LAN. This was the cause of a very annoying bug. I spent hours trying to figure out why it 
		// looked like I had 200ms of ping on my local network. Turns out it was because I was using an if statement instead of a while, so I was only getting
		// 1 packet per tick. UMMMMMMM :-(. Be warned future self, if you ever use similar netcode.
		while (recvfrom(sock, (char*)buffer, (MAX_CLIENTS * 13 + 5), 0, (SOCKADDR*)&recvAddress, &recvAddressSize) != SOCKET_ERROR) {
			timeOfLastServerMsg = SDL_GetTicks();

			switch (buffer[0]) {
				case (byte)serverDataTypes::joinResult:
					// If the server let us in then store our id
					if (buffer[1] == 1) {
						ID = buffer[2];
						connectedToServer = true;
						cout << "Connected to server\n";
						SDL_SetRenderDrawColor(renderer, 201, 254, 237, 255);
					}
					// Otherwise you can just vibe like the hacker you are - unless they aren't a hacker. That means the erver is just full
					else {
						SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Something went wrong.", "The server denied your join request.\nThis is probably because the server is full.\nPlease play again later.", window);
						return true;
					}
				break;

				case (byte)serverDataTypes::gameState:
					// The data received from the server in the form of a buffer has a certain structure. The first byte indicates that the data is the game state
					// Second byte indicates number of connected clients in the current game. Then there are sections of 13 bytes for each client.
					// The first byte of a section specifies the client's ID The next 8 bytes hold the x and y positions of the player (in the form of 4-byte integers).
					// The last 4 bytes holds the ID of the client-input-message that the server has most recently processed. Used in the client side prediction

					connectedPlayers = buffer[1];
					//cout << "Received game state from server.\nconnectedPlayers: " << (int)connectedPlayers << "\nx-coord: " << (int)buffer[2 + 0 * 9] << "\ny-coord: " << (int)buffer[6 + 0 * 9] << endl;

					byte aLittleVariableThatNobodyCaresAbout = 1;
					for (byte i = 0; i < connectedPlayers; i++) {
						// Get the input number for this state
						unsigned int lastProcessedInputNum = 0;
						memcpy(&lastProcessedInputNum, &buffer[11 + i * 13], 4);

						// If this client is us then we need to do some prediction stuff
						if (ID == buffer[2 + i * 13]) {
							// Copy the position of me to the map
							memcpy(&players[0].xPos, &buffer[3 + i * 13], 4);
							memcpy(&players[0].yPos, &buffer[7 + i * 13], 4);

							// Since the server is authoritative and we just updated the client position with what it sent to us, we also need to reapply all
							// inputs that the server has not yet processed. For example, say we had lots of lag, 300ms, and we moved twice to the right. Because
							// we're doing CSP, we move the client instantly, so he gets to the right before the server data comes back for just the first key
							// press. When it does get back, the player will move backwards and then forwards again as the second key press comes in. To solve
							// this we do server reconciliationn: https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html

							int clientInputIndex = 0;
							while (clientInputIndex < inputs.size()) {
								// If the server has processed this input we dont care about it any more so we can delete it
								if (inputs[clientInputIndex].inputNum <= lastProcessedInputNum)
									inputs.erase(inputs.begin() + clientInputIndex);
								// If the server has not processed this input yet we need to reapply it to keep up the illusion of 0 lag (CSP)
								else {
									byte movementByte = inputs[clientInputIndex].movementByte;
									if (movementByte & 1)
										players[0].yPos -= (int)(450 * inputs[clientInputIndex].dt);
									if (movementByte & 2)
										players[0].xPos -= (int)(450 * inputs[clientInputIndex].dt);
									if (movementByte & 4)
										players[0].yPos += (int)(450 * inputs[clientInputIndex].dt);
									if (movementByte & 8)
										players[0].xPos += (int)(450 * inputs[clientInputIndex].dt);

									clientInputIndex++;
								}
							}
						}
						else {
							memcpy(&players[aLittleVariableThatNobodyCaresAbout].xPos, &buffer[3 + i * 13], 4);
							memcpy(&players[aLittleVariableThatNobodyCaresAbout].yPos, &buffer[7 + i * 13], 4);
							aLittleVariableThatNobodyCaresAbout++;
						}
					}
				break;
			}
		}

		// Draw stuff ///////////////////////////////
		SDL_RenderClear(renderer);

		// Draw every player
		for (byte i = 0; i < connectedPlayers; i++) {
			SDL_Rect destRect = { players[i].xPos, players[i].yPos, 32, 32 };
			SDL_RenderCopy(renderer, playerTexture, NULL, &destRect);
		}

		SDL_RenderPresent(renderer);
		//////////////////////////////////////////

		// If we are connected, send server our input, even if there are no keys being pressed //////////////////
		if (connectedToServer == true) {
			//cout << "Yooooo\n";
			float elapsedTime = SDL_GetTicks() / 1000.0f - timeOfPrevFrame;
			timeOfPrevFrame = SDL_GetTicks() / 1000.0f;

			const Uint8* keyboardState = SDL_GetKeyboardState(NULL);
			Uint8 inputByte = 0;

			// FOR CLIENT SIDE PREDICTION:
			// When we send the data off to the server, we can update our local representation of the client. This will later be corrected
			// (if it needs to be corrected) by the server when it sends us back the game state

			// Set the first 4 bits to 1 based on which keys are pressed
			if (keyboardState[SDL_SCANCODE_W]) {
				inputByte |= 1;
				players[0].yPos -= (int)(450 * elapsedTime);
			}
			if (keyboardState[SDL_SCANCODE_A]) {
				inputByte |= 2;
				players[0].xPos -= (int)(450 * elapsedTime);
			}
			if (keyboardState[SDL_SCANCODE_S]) {
				inputByte |= 4;
				players[0].yPos += (int)(450 * elapsedTime);
			}
			if (keyboardState[SDL_SCANCODE_D]) {
				inputByte |= 8;
				players[0].xPos += (int)(450 * elapsedTime);
			}

			buffer[0] = (byte)clientDataTypes::input;
			buffer[1] = ID;
			buffer[2] = inputByte;

			// The server needs to know the time since the last frame so they can move us properly
			memcpy(&buffer[3], &elapsedTime, sizeof(elapsedTime));
			// When the server sends the game state back to us, it will include the input/sequence number so we can replay unhandled inputs
			memcpy(&buffer[7], &clientMsgNum, 4);
			// Send to the server
			if (sendto(sock, (char*)buffer, 11, 0, (SOCKADDR*)&serverAddress, sizeof(serverAddress)) == SOCKET_ERROR)
				cout << WSAGetLastError() << endl;

			// Add this input to the vector for later use when we receive the confirmation from the server
			clientInput input = {inputByte, elapsedTime, clientMsgNum};
			inputs.push_back(input);
			clientMsgNum++;
		}
		//////////////////////////////////////////

		// If we havent received stuff in a while then the network has probably crashed. We can inform the user and disconnect from the server
		if (SDL_GetTicks() - timeOfLastServerMsg > 5000) {
			connectedToServer = false;
			onMenuScreen = true;
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Error", "The server is not responding - you have been diconnected.\nPlease check your network connection and try again.", window);
		}

		buffer[0] = 0;
		return false;
	}

	void startGameLoop() {
		timeOfPrevFrame = SDL_GetTicks() / 1000.0f;
		bool quit = false;
		while (!quit) {
			quit = doFrame();
			frameCount++;
		}

		// Send neat leave message to server so the client slot can be reused instantly
		if (connectedToServer == true) {
			buffer[0] = (byte)clientDataTypes::leave;
			buffer[1] = ID;
			sendto(sock, (char*)buffer, 2, 0, (SOCKADDR*)&serverAddress, sizeof(serverAddress));
		}
	}

private:
	SOCKET sock;
	SOCKADDR_IN serverAddress;

	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* playerTexture;
	SDL_Texture* clickToPlayTexture;

	// Simple 2d vector for player position
	struct player {
		int xPos;
		int yPos;
	};

	// A struct containing data for a client input. The inputs vector uses this for it's elements
	struct clientInput {
		byte movementByte;
		float dt;
		unsigned int inputNum;
	};

	// All of the players in the game. players[0] is me/us/this client
	player players[CLIENTS_PER_GAME] = {{}};
	// This vector is filled with the inputs that we have sent to the server. Used for CSP. We can loop over these inputs, and discard or re-apply them as needed
	vector<clientInput> inputs;
	// How many are currently connected
	byte connectedPlayers;

	byte ID;
	bool connectedToServer;
	unsigned int timeOfLastServerMsg;
	bool onMenuScreen;

	byte buffer[MAX_CLIENTS * 13 + 5] = {};

	float timeOfPrevFrame;
	// This is incremented every message.
	unsigned int clientMsgNum;

	// The client will send 1 of 3 possible bytes at the start of their message
	enum class clientDataTypes {
		join = 1,   // Client is joining for the first time
		leave = 2,  // Client is leaving
		input = 3   // Client is sending their input to the server
	};

	// The server will send one of these 2 messages to the client
	enum class serverDataTypes {
		joinResult = 1,  // Client has previously requested to join the game, so the server sends back accepted (along with client ID) or denied
		gameState = 2    // Server is sending game state to client
	};

	unsigned long frameCount;
};

int main(int argc, char* args[]) {
	Game game = Game();
	game.initSDL();
	game.initWinsock();
	game.load();
	game.startGameLoop();

	return 0;
}