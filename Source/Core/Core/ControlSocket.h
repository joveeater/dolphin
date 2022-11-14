#pragma once

#include <vector>
#include <memory>
#include <thread>

#include <SFML/Network.hpp>

struct ConnInfo{
	std::unique_ptr<sf::TcpSocket> sock;
	size_t curRead;
	size_t packetSize;
	char * buffer;


	char * sendBase;
	char * sendLow;
	char * sendHigh;
	
	size_t sendSize;
};

class ControlSocket {

	public:
		ControlSocket(unsigned short p, const sf::IpAddress & addr = sf::IpAddress::Any) : port(p), address(addr) {};
		
		void run();
		void close();
		void sendState(bool force_reg=false);
		void refreshBp();
		
	private:
		void run_thread();
		void parseBuffer(char * buffer, size_t size);
		
		unsigned short port;
		bool running=false;
		const sf::IpAddress& address;
		sf::TcpListener listener;
		std::vector<std::unique_ptr<ConnInfo>> connections;
		std::thread sock_thread;
};
