#include <winsock2.h>
#include <Ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <stdlib.h>
#include <chrono>

using namespace std;

const float serverFrequency = 60.0;

int thingVariable = 0;

#define MAX_CLIENTS 255

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

// A data structure for everything that a player/client contains
struct player {
	unsigned int address;
	unsigned short port;

	int x;
	int y;

	bool pressingUp;
	bool pressingLeft;
	bool pressingDown;
	bool pressingRight;

	// This holds the number of the latest client input the server has handled. Used in client side prediction
	unsigned int lastProcessedInputNumber;
};

int main() {
	WSAData winsockData;
	if (WSAStartup(0x202, &winsockData)) {
		cout << "Failed to start winsock: " << WSAGetLastError();
		return 1;
	}

	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET) {
		cout << "Couldn't create the UDP socket: " << WSAGetLastError();
		return 1;
	}

	SOCKADDR_IN address;
	address.sin_family = AF_INET;
	// Make sure port is stored as big endian with htons()
	address.sin_port = htons(5555);
	address.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (SOCKADDR*)&address, sizeof(address)) == SOCKET_ERROR) {
		cout << "Couldn't bind the socket: " << WSAGetLastError();
		return 1;
	}

	// Put socket in non-blocking mode
	u_long argp = 1;
	ioctlsocket(sock, FIONBIO, &argp);


	// This will make windows check to wake up thread every 1 ms
	if (timeBeginPeriod(1) != TIMERR_NOERROR) {
		cout << "Couldn't set granularity of scheduler";
		return 1;
	}

	// Need to get frequency of the clock to convert the time to seconds
	LARGE_INTEGER counterFrequency;
	QueryPerformanceFrequency(&counterFrequency);


	byte buffer[MAX_CLIENTS * 13 + 5] = {};

	player players[MAX_CLIENTS];
	// This will let us decide if we havent heard from a player in a while. We can then disconnect them, assuming they've crashed or something
	float timeSinceLastClientMsg[MAX_CLIENTS];

	// Set every player address to 0. Makes it easier to check for empty client positions
	for (byte i = 0; i < MAX_CLIENTS; i++) {
		players[i].address = 0;
		timeSinceLastClientMsg[i] = 0.0f;
	}

	bool quit = false;
	while (!quit) {
		// Get the current time of the loop in whatever unit the clock measures it in
		LARGE_INTEGER startTime;
		QueryPerformanceCounter(&startTime);

		SOCKADDR_IN recvAddress;
		int recvAddressSize = sizeof(recvAddress);

		//!!!!!!!!!!!!!! VERY IMPORTANT NOTE !!!!!!!!!!!!!//
		// Need a while loop to recv ALL packets from the client. This is very important. If we don't do this, some packets won't be received this tick.
		// This creates the appearance of high ping, even in a LAN. This was the cause of a very annoying bug. I spent hours trying to figure out why it 
		// looked like I had 200ms of ping on my local network. Turns out it was because I was using an if statement instead of a while, so I was only getting
		// 1 packet per tick. Big shame. Be warned future self, if you ever use similar netcode.
		while (recvfrom(sock, (char*)buffer, 11, 0, (SOCKADDR*)&recvAddress, &recvAddressSize) != SOCKET_ERROR) {
			byte clientID;
			bool clientIDSet = false;

			// The first byte will hold the type of data the client is sending.
			switch (buffer[0]) {
				case (byte)clientDataTypes::join:
					//cout << "Client joined\n";
					clientID = 0;

					// Go through every client to check their address
					for (byte i = 0; i < MAX_CLIENTS; i++) {
						// If we already have the client's address in our array, then this client is forging join messages for some reason, maybe they tryin
						// to dos the server. In that case, we shouldn't let that player join because they could do bad stuff.
						if (recvAddress.sin_addr.S_un.S_addr == players[i].address && recvAddress.sin_port == players[i].port) break;

						// If they aren't hacking, and we found a slot with the address as 0,
						// then it means we have found an empty client ID. We can give this one to the client.
						if (players[i].address == 0) {
							clientID = i;
							clientIDSet = true;
						}
					}

					buffer[0] = (byte)serverDataTypes::joinResult;
					buffer[1] = (byte)clientIDSet;
					buffer[2] = clientID;

					// Send the data back to the client. If the client ID was succesfully set then we will have a new client joining.
					// We need to add them to the players array.
					if (sendto(sock, (char*)buffer, 3, 0, (SOCKADDR*)&recvAddress, sizeof(recvAddress)) != SOCKET_ERROR && clientIDSet == true) {
						// Initialize player input and position to zero
						players[clientID] = { recvAddress.sin_addr.S_un.S_addr, recvAddress.sin_port, 0, 0, 0, 0, 0, 0};
						cout << "Client joined.   Address: " << recvAddress.sin_addr.S_un.S_addr << "   Port: " << recvAddress.sin_port << endl;
						timeSinceLastClientMsg[clientID] = 0.0f;
					}
				break;

				case (byte)clientDataTypes::leave:
					cout << "Client leaving\n";
					clientID = buffer[1];

					// Make sure that the client isn't pretending to be another player. Otherwise they could just go around disconnecting everyone.
					// We can then reset all of their variables
					if (recvAddress.sin_addr.S_un.S_addr == players[clientID].address && recvAddress.sin_port == players[clientID].port)
						players[clientID] = {};

				break;

				case (byte)clientDataTypes::input:
					//cout << "Client sending input\n";
					clientID = buffer[1];
					// Make sure that the client isn't pretending to be another player. Otherwise they could move other players
					if (recvAddress.sin_addr.S_un.S_addr == players[clientID].address && recvAddress.sin_port == players[clientID].port) {

						// Handle player input
						byte movementByte = buffer[2];

						players[clientID].pressingUp = movementByte & 1;
						players[clientID].pressingLeft = movementByte & 2;
						players[clientID].pressingDown = movementByte & 4;
						players[clientID].pressingRight = movementByte & 8;

						// We can reset the timeout timer since we have received stuff from the client
						timeSinceLastClientMsg[clientID] = 0.0f;

						// This is the time since the clients last frame. Use this to move them independent of frame rate
						float dt = 0;
						memcpy(&dt, &buffer[3], 4);

						if (movementByte & 1) {
							/*auto duration = chrono::system_clock::now().time_since_epoch();
							cout << "Handling W key press at time " << chrono::duration_cast<chrono::milliseconds>(duration).count() << endl;*/
							players[clientID].y -= (int)(450 * dt);
						}
						if (movementByte & 2)
							players[clientID].x -= (int)(450 * dt);
						if (movementByte & 4)
							players[clientID].y += (int)(450 * dt);
						if (movementByte & 8)
							players[clientID].x += (int)(450 * dt);

						// Get the ID of the client input. Used for client side prediction
						memcpy(&players[clientID].lastProcessedInputNumber, &buffer[7], 4);
					}

				break;
			}
		}

		// Move the players
		for (byte i = 0; i < MAX_CLIENTS; i++) {
			if (players[i].address == 0) continue;

			// If we haven't heard from a client in the last 5 seconds they have probably disconnected from a game or network crash, or just a rage quit lol
			timeSinceLastClientMsg[i] += 1.0f / serverFrequency;
			if (timeSinceLastClientMsg[i] > 5)
				// Reset player variables
				players[i] = {};

			/*if (players[i].pressingUp) {
				//auto duration = chrono::system_clock::now().time_since_epoch();
				//cout << "Handling W key press at time " << chrono::duration_cast<chrono::milliseconds>(duration).count() << endl;
				players[i].y -= (int)(300 / serverFrequency);
			}
			if (players[i].pressingLeft)
				players[i].x -= (int)(300 / serverFrequency);
			if (players[i].pressingDown)
				players[i].y += (int)(300 / serverFrequency);
			if (players[i].pressingRight) {
				players[i].x += (int)(300 / serverFrequency);
				thingVariable += 5;
				//thingVariable = rand() % 100;
				//cout << "Pressing right: " << thingVariable << endl;
			}

			timeSinceLastClientMsg[i] += 1.0f / serverFrequency;
			if (timeSinceLastClientMsg[i] > 5)
				players[i] = {};*/
		}

		// Now we can create the state packet. We can't do this in the previous loop because a player with a bigger ID coud push a player with a smaller ID,
		// and it wouldn't get put in. The clients would then be out of sync for a bit.
		// The first byte of the packet indicates that it is a game state packet
		buffer[0] = (byte)serverDataTypes::gameState;
		// Start at a 2-byte offset. The first byte contains the data for the above line of code,
		// and the second byte is resreved for the number of connected players
		unsigned short bytesWritten = 2;
		byte connectedClients = 0;

		for (byte i = 0; i < MAX_CLIENTS; i++) {
			// No use for empty client
			if (players[i].address == 0) continue;

			// After the first byte, there are sections of 13 bytes for each client. The first byte of a section specifies the client's ID
			buffer[bytesWritten] = i;
			bytesWritten += 1;

			// The next 4 bytes hold the x-position of the client
			memcpy(&buffer[bytesWritten], &players[i].x, 4);
			bytesWritten += 4;
			
			// Next 4 bytes hold the y-position of the player
			memcpy(&buffer[bytesWritten], &players[i].y, 4);
			bytesWritten += 4;

			// Last 4 bytes hold the ID of the most recent client-input message the server has processed
			memcpy(&buffer[bytesWritten], &players[i].lastProcessedInputNumber, 4);
			bytesWritten += 4;

			connectedClients++;
		}
		buffer[1] = connectedClients;

		SOCKADDR_IN sendAddress;
		sendAddress.sin_family = AF_INET;

		for (byte i = 0; i < MAX_CLIENTS; i++) {
			// No use for empty client
			if (players[i].address == 0) continue;

			sendAddress.sin_port = players[i].port;
			sendAddress.sin_addr.S_un.S_addr = players[i].address;

			//cout << "Player x: " << (int)buffer[2] << endl;
			//int playerX = players[i].x;
			//cout << "playerX: " << playerX << endl;
			if (sendto(sock, (const char*)&buffer, bytesWritten, 0, (SOCKADDR*)&sendAddress, sizeof(sendAddress)) == SOCKET_ERROR) {
				cout << "senderr:  " << WSAGetLastError() << endl;
			}

			//int var = rand() % 100;
			//if (var == 4) {
				//cout << "sending " << thingVariable << endl;
				//byte buf[5];
				//buf[0] = 5;
				//int v = 5;
				//memcpy(&buf[1], &thingVariable, 4);
				//cout << "Buffer: " << (int)buf[1] << endl;
				//sendto(sock, (char*)&thingVariable, 5, 0, (SOCKADDR*)&sendAddress, sizeof(sendAddress));
				//cout << "Sent\n";
			//}
		}

		// Reset command
		buffer[0] = 0;

		// Get the time now
		LARGE_INTEGER endTime;
		QueryPerformanceCounter(&endTime);
		// We can then subtract the starting time to get the elapsed time. However, this will be in a unit that is decided by the system (I think)
		// so we need to divide it by the frequency to convert the elapsed time to seconds. It works because the frequency tells us how many
		// counts per second the counter is running at.
		float elapsedTime = (float)(endTime.QuadPart - startTime.QuadPart) / (float)counterFrequency.QuadPart;

		// Since we want a fixed tick rate, we loop and sleep until we get the desired rate
		while (elapsedTime < 1.0 / serverFrequency) {
			// Now we can sleep until we need to start up again. To get the time in seconds to sleep, we need to find how long until the next tick.
			// Subtract the elapsed time from the server frequency. The sleep function takes ms, so we need to multiply it by 1000
			DWORD sleepingTime = DWORD((1.0 / serverFrequency - elapsedTime) * 1000);
			//cout << "Sleeping for " << sleepingTime << "ms\n";
			if (sleepingTime > 0) Sleep(sleepingTime);

			// Update the elapsed time. This is because the sleep function cant go less than 1 ms. For example, if there is 1.75 ms until the next tick,
			// We will sleep for 1 ms but there will still be that 0.75 left over. For that, we just spin in the while loop
			QueryPerformanceCounter(&endTime);
			elapsedTime = (float)(endTime.QuadPart - startTime.QuadPart) / (float)counterFrequency.QuadPart;
		}
	}

	timeEndPeriod(1);
	WSACleanup();

	return 0;
}