#include "ControlSocket.h"

#include <cstring>

#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Core.h"

static void flushSend(ConnInfo & info){
	if(info.sendLow==info.sendHigh){
		return;
	}
	if(info.sendLow<info.sendHigh){
		size_t sent;
		info.sock->send(info.sendLow, info.sendHigh-info.sendLow, sent);
		info.sendLow+=sent;
	}else{
		size_t sent;
		info.sock->send(info.sendLow, info.sendBase+info.sendSize-info.sendLow, sent);
		int s=sent;
		if(s!=info.sendBase+info.sendSize-info.sendLow){
			info.sendLow+=sent;
			return;
		}
		info.sendLow=info.sendBase;
		flushSend(info);
	}
}

static void sendMessage(ConnInfo & info, char * buffer){
	size_t i=strlen(buffer);

	int used=info.sendHigh-info.sendLow;
	if(used<0){
		used+=info.sendSize;
	}

	while(used+i+8>=info.sendSize){
		char * newbuf=(char*)malloc(info.sendSize*2);
		size_t copied;
		if(info.sendHigh>=info.sendLow){
			memcpy(newbuf, info.sendLow, info.sendHigh-info.sendLow);
			copied=info.sendHigh-info.sendLow;
		}else{
			memcpy(newbuf, info.sendLow, info.sendBase+info.sendSize-info.sendLow);
			copied=info.sendBase+info.sendSize-info.sendLow;
			memcpy(newbuf+copied, info.sendBase, info.sendHigh-info.sendBase);
			copied+=info.sendHigh-info.sendBase;
		}
		free(info.sendBase);
		info.sendLow=info.sendBase=newbuf;
		info.sendHigh=info.sendBase+copied;
		info.sendSize*=2;
	}

	if(info.sendHigh+sizeof(size_t)>=info.sendBase+info.sendSize){
		memcpy(info.sendHigh, &i,(info.sendBase+info.sendSize)-info.sendHigh);
		size_t copied=(info.sendBase+info.sendSize)-info.sendHigh;
		
		memcpy(info.sendBase, reinterpret_cast<char*>(&i)+copied, sizeof(size_t)-copied);
		info.sendHigh=info.sendBase+(sizeof(size_t)-copied);
	}else{
		memcpy(info.sendHigh, &i, sizeof(size_t));
		info.sendHigh+=sizeof(size_t);
	}
	
	if(info.sendHigh+i>=info.sendBase+info.sendSize){
		memcpy(info.sendHigh, buffer, (info.sendBase+info.sendSize)-info.sendHigh);
		size_t copied=(info.sendBase+info.sendSize)-info.sendHigh;

		memcpy(info.sendBase, buffer+copied, i-copied);
		info.sendHigh=info.sendBase+(i-copied);
	}else{
		memcpy(info.sendHigh, buffer, i);
		info.sendHigh+=i;
	}

	flushSend(info);
}

static void sockSendBpRefresh(ConnInfo & info){
	sendMessage(info, "BPREFRESH:");
}

static void sockSendRegisters(ConnInfo & info){
	char buf[16384];
	buf[16383]=0;
	char * cur=buf;
	cur+=snprintf(cur, 16383-(cur-buf), "REG: %x ", PowerPC::ppcState.pc);
	cur+=snprintf(cur, 16383-(cur-buf), "%x ", PowerPC::ppcState.spr[SPR_LR]);
	for(int i=0; i<32; i++){
		cur+=snprintf(cur, 16383-(cur-buf), "%x ", GPR(i));
	}
	for(int i=0; i<32; i++){
		cur+=snprintf(cur, 16383-(cur-buf), "%lx %lx ", rPS(i).PS0AsU64(), rPS(i).PS1AsU64());
	}
	sendMessage(info, buf);
}

static void sockSendState(ConnInfo & info){
	CPU::State s=CPU::GetState();
	char buf[4096];
	buf[4095]=0;
	if(s==CPU::State::PowerDown){
		snprintf(buf, 4095, "STATE: %s %d", "OFF", getpid());
		sendMessage(info, buf);
		return;
	}

	snprintf(buf, 4095, "STATE: %s %d %p %x", s==CPU::State::Stepping? "PAUSED" : "RUNNING", getpid(), Memory::m_pRAM, Memory::GetRamSize());
	sendMessage(info, buf);
	if(s==CPU::State::Stepping){
		sockSendRegisters(info);
	}
}

void ControlSocket::run_thread(){
	if(listener.listen(port, address)!=sf::Socket::Status::Done){
		return;
	}

	Core::AddOnCPUStateChangedCallback([&]()->void{
		this->sendState(false);
	});

	PowerPC::breakpoints.addHandler([&]()->void{
		this->sendState(true);
	});
	
	Core::AddOnBpClearCallback([&]()->void{
		this->refreshBp();
	});
	
	listener.setBlocking(false);

	auto new_client = std::make_unique<sf::TcpSocket>();
	while(running){
		if(listener.accept(*new_client) == sf::Socket::Done){
			new_client->setBlocking(false);
			char * outbuf=(char*)malloc(4096);
			std::unique_ptr<ConnInfo> ci(new ConnInfo(std::move(new_client), 0, 0, nullptr, outbuf, outbuf, outbuf, 4096));

			sockSendState(*ci);
			connections.push_back(std::move(ci));
			new_client = std::make_unique<sf::TcpSocket>();
		}

		auto it=connections.begin();

		while(it!=connections.end()){

			if((*it)->curRead<sizeof(size_t)){
				size_t recvd;
				sf::Socket::Status stat=(*it)->sock->receive(reinterpret_cast<char*>(&((*it)->packetSize))+(*it)->curRead, sizeof(size_t)-(*it)->curRead, recvd);

				if(stat==sf::Socket::Status::Done){
					if(recvd<(sizeof(size_t)-(*it)->curRead)){
						(*it)->curRead+=recvd;
						continue;
					}
					(*it)->curRead+=recvd;
					(*it)->buffer=(char*)malloc((*it)->packetSize+1);
					(*it)->buffer[(*it)->packetSize]=0;
				}else if(stat==sf::Socket::Status::Disconnected || stat==sf::Socket::Status::Error){
					(*it)->sock->disconnect();
					if((*it)->buffer){
						free((*it)->buffer);
						(*it)->buffer=nullptr;
					}
					it=connections.erase(it);
					continue;
				}	
			}
			
			if((*it)->packetSize && (*it)->curRead>=sizeof(size_t)){
				size_t recvd;
				sf::Socket::Status stat=(*it)->sock->receive((*it)->buffer+(*it)->curRead-sizeof(size_t), sizeof(size_t)+(*it)->packetSize-(*it)->curRead, recvd);
				if(stat==sf::Socket::Status::Done){
					if(recvd<sizeof(size_t)+((*it)->packetSize-(*it)->curRead)){
						(*it)->curRead+=recvd;
						continue;
					}
					parseBuffer((*it)->buffer, (*it)->packetSize);
					free((*it)->buffer);
					(*it)->buffer=nullptr;
					(*it)->curRead=0;
					(*it)->packetSize=0;
				}else if(stat==sf::Socket::Status::Disconnected || stat==sf::Socket::Status::Error){
					(*it)->sock->disconnect();
					if((*it)->buffer){
						free((*it)->buffer);
						(*it)->buffer=nullptr;
					}
					it=connections.erase(it);
					continue;
				}
			}

			it++;
		}
	}

	listener.close();

	for(auto& con : connections){
		con->sock->disconnect();
	}
	connections.clear();
}

void ControlSocket::parseBuffer(char * buffer, size_t size){
	uint32_t bp;
	if(sscanf(buffer, "ADDBP: %d", &bp)>0){
		PowerPC::breakpoints.Add(bp, false, false, true, std::nullopt);
	}else if(sscanf(buffer, "DELBP: %d", &bp)>0){
		PowerPC::breakpoints.Remove(bp);
	}else if(sscanf(buffer, "ADDWP: %d", &bp)>0){
		//NYI
	}else if(sscanf(buffer, "DELWP: %d", &bp)>0){
		PowerPC::memchecks.Remove(bp);
	}else if(strncmp(buffer, "START:", strlen("START:"))==0){
		Core::SetState(Core::State::Running);
	}
}

void ControlSocket::sendState(bool force_reg){
	for(auto& con : connections){
		if(force_reg){
			sockSendRegisters(*con);
		}else{
			sockSendState(*con);
		}
	}
}

void ControlSocket::refreshBp(){
	for(auto& con : connections){
		sockSendBpRefresh(*con);
	}
}

void ControlSocket::run(){
	if(running){
		return;
	}
	running=true;
	sock_thread=std::thread(&ControlSocket::run_thread, this);
}

void ControlSocket::close(){
	running=false;
}
