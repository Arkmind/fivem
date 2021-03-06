/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "NetLibrary.h"
#include <base64.h>
#include "ICoreGameInit.h"
#include <mutex>
#include <mmsystem.h>
#include <yaml-cpp/yaml.h>
#include <SteamComponentAPI.h>

#include <Error.h>

inline ISteamComponent* GetSteam()
{
	auto steamComponent = Instance<ISteamComponent>::Get();

	// if Steam isn't running, return an error
	if (!steamComponent->IsSteamRunning())
	{
		steamComponent->Initialize();

		if (!steamComponent->IsSteamRunning())
		{
			return nullptr;
		}
	}

	return steamComponent;
}

static uint32_t m_tempGuid = GetTickCount();

uint16_t NetLibrary::GetServerNetID()
{
	return m_serverNetID;
}

uint16_t NetLibrary::GetHostNetID()
{
	return m_hostNetID;
}

void NetLibrary::ProcessPackets()
{
	ProcessPacketsInternal(NA_INET4);
	ProcessPacketsInternal(NA_INET6);
}

void NetLibrary::ProcessPacketsInternal(NetAddressType addrType)
{
	char buf[2048];
	memset(buf, 0, sizeof(buf));

	sockaddr_storage from;
	memset(&from, 0, sizeof(from));

	int fromlen = sizeof(from);

	auto socket = (addrType == NA_INET4) ? m_socket : m_socket6;

	while (true)
	{
		int len = recvfrom(socket, buf, 2048, 0, (sockaddr*)&from, &fromlen);

		NetAddress fromAddr((sockaddr*)&from);

		if (len == SOCKET_ERROR)
		{
			int error = WSAGetLastError();

			if (error != WSAEWOULDBLOCK)
			{
				trace("recv() failed - %d\n", error);
			}

			return;
		}

		if (*(int*)buf == -1)
		{
			ProcessOOB(fromAddr, &buf[4], len - 4);
		}
		else
		{
			if (fromAddr != m_currentServer)
			{
				trace("invalid from address for server msg\n");
				return;
			}

			NetBuffer* msg;

			if (m_netChannel.Process(buf, len, &msg))
			{
				ProcessServerMessage(*msg);

				delete msg;
			}
		}
	}
}

void NetLibrary::ProcessServerMessage(NetBuffer& msg)
{
	// update received-at time
	m_lastReceivedAt = GetTickCount();

	// metrics bits
	NetPacketMetrics metrics;

	// receive the message
	uint32_t msgType;

	uint32_t curReliableAck = msg.Read<uint32_t>();

	if (curReliableAck != m_outReliableAcknowledged)
	{
		for (auto it = m_outReliableCommands.begin(); it != m_outReliableCommands.end();)
		{
			if (it->id <= curReliableAck)
			{
				it = m_outReliableCommands.erase(it);
			}
			else
			{
				it++;
			}
		}

		m_outReliableAcknowledged = curReliableAck;
	}

	if (m_connectionState == CS_CONNECTED)
	{
		m_connectionState = CS_ACTIVE;
	}

	if (m_connectionState != CS_ACTIVE)
	{
		return;
	}

	do
	{
		if (msg.End())
		{
			break;
		}

		msgType = msg.Read<uint32_t>();

		if (msgType == 0xE938445B) // 'msgRoute'
		{
			uint16_t netID = msg.Read<uint16_t>();
			uint16_t rlength = msg.Read<uint16_t>();

			//trace("msgRoute from %d len %d\n", netID, rlength);

			char routeBuffer[65536];
			if (!msg.Read(routeBuffer, rlength))
			{
				break;
			}

			EnqueueRoutedPacket(netID, std::string(routeBuffer, rlength));

			// add to metrics
			metrics.AddElementSize(NET_PACKET_SUB_ROUTED_MESSAGES, 2 + rlength);
		}
		else if (msgType == 0x53FFFA3F) // msgFrame
		{
			// for now, frames are only an identifier - this will change once game features get moved to our code
			// (2014-10-15)

			uint32_t frameNum = msg.Read<uint32_t>();

			m_lastFrameNumber = frameNum;

			// handle ping status
			if (m_serverProtocol >= 3)
			{
				int currentPing = msg.Read<int>();

				if (m_metricSink.GetRef())
				{
					m_metricSink->OnPingResult(currentPing);
				}
			}
		}
		else if (msgType != 0xCA569E63) // reliable command
		{
			uint32_t id = msg.Read<uint32_t>();
			uint32_t size;

			if (id & 0x80000000)
			{
				size = msg.Read<uint32_t>();
				id &= ~0x80000000;

				metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4);
			}
			else
			{
				size = msg.Read<uint16_t>();

				metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 2);
			}

			// test for bad scenarios
			if (id > (m_lastReceivedReliableCommand + 64))
			{
				return;
			}

			char* reliableBuf = new(std::nothrow) char[size];

			if (!reliableBuf)
			{
				return;
			}

			if (!msg.Read(reliableBuf, size))
			{
				break;
			}

			// check to prevent double execution
			if (id > m_lastReceivedReliableCommand)
			{
				HandleReliableCommand(msgType, reliableBuf, size);

				m_lastReceivedReliableCommand = id;
			}

			delete[] reliableBuf;

			// add to metrics
			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4 + size);
		}
	} while (msgType != 0xCA569E63); // 'msgEnd'

	if (m_metricSink.GetRef())
	{
		m_metricSink->OnIncomingPacket(metrics);
	}
}

bool NetLibrary::WaitForRoutedPacket(uint32_t timeout)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (!m_incomingPackets.empty())
		{
			return true;
		}
	}

	WaitForSingleObject(m_receiveEvent, timeout);

	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		return (!m_incomingPackets.empty());
	}
}

void NetLibrary::EnqueueRoutedPacket(uint16_t netID, std::string packet)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		RoutingPacket routePacket;
		routePacket.netID = netID;
		routePacket.payload = packet;
		routePacket.genTime = timeGetTime();

		m_incomingPackets.push(routePacket);
	}

	SetEvent(m_receiveEvent);
}

bool NetLibrary::DequeueRoutedPacket(char* buffer, size_t* length, uint16_t* netID)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (m_incomingPackets.empty())
		{
			return false;
		}

		auto packet = m_incomingPackets.front();
		m_incomingPackets.pop();

		memcpy(buffer, packet.payload.c_str(), packet.payload.size());
		*netID = packet.netID;
		*length = packet.payload.size();

		// store metrics
		auto timeval = (timeGetTime() - packet.genTime);

		m_metricSink->OnRouteDelayResult(timeval);
	}

	ResetEvent(m_receiveEvent);

	return true;
}

void NetLibrary::RoutePacket(const char* buffer, size_t length, uint16_t netID)
{
	RoutingPacket routePacket;
	routePacket.netID = netID;
	routePacket.payload = std::string(buffer, length);

	m_outgoingPackets.push(routePacket);
}

#define	BIG_INFO_STRING		8192  // used for system info key only
#define	BIG_INFO_KEY		  8192
#define	BIG_INFO_VALUE		8192

/*
===============
Info_ValueForKey

Searches the string for the given
key and returns the associated value, or an empty string.
FIXME: overflow check?
===============
*/
char *Info_ValueForKey(const char *s, const char *key)
{
	char	pkey[BIG_INFO_KEY];
	static	char value[2][BIG_INFO_VALUE];	// use two buffers so compares
											// work without stomping on each other
	static	int	valueindex = 0;
	char	*o;

	if (!s || !key)
	{
		return "";
	}

	if (strlen(s) >= BIG_INFO_STRING)
	{
		return "";
	}

	valueindex ^= 1;
	if (*s == '\\')
		s++;
	while (1)
	{
		o = pkey;
		while (*s != '\\')
		{
			if (!*s)
				return "";
			*o++ = *s++;
		}
		*o = 0;
		s++;

		o = value[valueindex];

		while (*s != '\\' && *s)
		{
			*o++ = *s++;
		}
		*o = 0;

		if (!_stricmp(key, pkey))
			return value[valueindex];

		if (!*s)
			break;
		s++;
	}

	return "";
}


#define Q_IsColorString( p )  ( ( p ) && *( p ) == '^' && *( ( p ) + 1 ) && isdigit( *( ( p ) + 1 ) ) ) // ^[0-9]

void StripColors(const char* in, char* out, int max)
{
	max--; // \0
	int current = 0;
	while (*in != 0 && current < max)
	{
		if (!Q_IsColorString(in))
		{
			*out = *in;
			out++;
			current++;
		}
		else
		{
			*in++;
		}
		*in++;
	}
	*out = '\0';
}

void NetLibrary::ProcessOOB(NetAddress& from, char* oob, size_t length)
{
	if (from == m_currentServer)
	{
		if (!_strnicmp(oob, "infoResponse", 12))
		{
			const char* infoString = &oob[13];

			m_infoString = infoString;

			{
				auto steam = GetSteam();

				if (steam)
				{
					char hostname[256] = { 0 };
					strncpy(hostname, Info_ValueForKey(infoString, "hostname"), 255);

					char cleaned[256];

					StripColors(hostname, cleaned, 256);

					steam->SetRichPresenceTemplate("{0}\n\n{2} on {3} with {1}");
					steam->SetRichPresenceValue(0, std::string(cleaned).substr(0, 64) + "...");
					steam->SetRichPresenceValue(1, "Connecting...");
					steam->SetRichPresenceValue(2, Info_ValueForKey(infoString, "gametype"));
					steam->SetRichPresenceValue(3, Info_ValueForKey(infoString, "mapname"));
				}
			}

			// until map reloading is in existence
			std::string thisWorld = Info_ValueForKey(infoString, "world");

			if (thisWorld.empty())
			{
				thisWorld = "gta5";
			}

			static std::string lastWorld = thisWorld;

			if (lastWorld != thisWorld && Instance<ICoreGameInit>::Get()->GetGameLoaded())
			{
				GlobalError("Was loaded in world %s, but this server is world %s. Restart the game to join.", lastWorld, thisWorld);
				return;
			}

			lastWorld = thisWorld;

			// finalize connecting
			m_connectionState = CS_CONNECTING;
			m_lastConnect = 0;
			m_connectAttempts = 0;
		}
		else if (!_strnicmp(oob, "connectOK", 9))
		{
			char* clientNetIDStr = &oob[10];
			char* hostIDStr = strchr(clientNetIDStr, ' ');

			hostIDStr[0] = '\0';
			hostIDStr++;

			char* hostBaseStr = strchr(hostIDStr, ' ');

			hostBaseStr[0] = '\0';
			hostBaseStr++;

			m_serverNetID = atoi(clientNetIDStr);
			m_hostNetID = atoi(hostIDStr);
			m_hostBase = atoi(hostBaseStr);

			m_lastReceivedReliableCommand = 0;

			trace("connectOK, our id %d, host id %d\n", m_serverNetID, m_hostNetID);

			OnConnectOKReceived(m_currentServer);

			m_netChannel.Reset(m_currentServer, this);
			m_connectionState = CS_CONNECTED;
		}
		else if (!_strnicmp(oob, "error", 5))
		{
			if (from != m_currentServer)
			{
				trace("Received 'error' request was not from the host\n");
				return;
			}

			if (length >= 6)
			{
				char* errorStr = &oob[6];

				GlobalError("%s", std::string(errorStr, length - 6));
			}
		}
	}
}

void NetLibrary::SetHost(uint16_t netID, uint32_t base)
{
	m_hostNetID = netID;
	m_hostBase = base;
}

void NetLibrary::SetBase(uint32_t base)
{
	m_serverBase = base;
}

uint32_t NetLibrary::GetHostBase()
{
	return m_hostBase;
}

void NetLibrary::SetMetricSink(fwRefContainer<INetMetricSink>& sink)
{
	m_metricSink = sink;
}

void NetLibrary::HandleReliableCommand(uint32_t msgType, const char* buf, size_t length)
{
	auto range = m_reliableHandlers.equal_range(msgType);

	std::for_each(range.first, range.second, [&] (std::pair<uint32_t, ReliableHandlerType> handler)
	{
		handler.second(buf, length);
	});
}

NetLibrary::RoutingPacket::RoutingPacket()
{
	//genTime = timeGetTime();
	genTime = 0;
}

void NetLibrary::ProcessSend()
{
	// is it time to send a packet yet?
	bool continueSend = false;

/*	if (GameFlags::GetFlag(GameFlag::InstantSendPackets))
	{
		if (!m_outgoingPackets.empty())
		{
			continueSend = true;
		}
	}*/

	if (!continueSend)
	{
		uint32_t diff = timeGetTime() - m_lastSend;

		if (diff >= (1000 / 60))
		{
			continueSend = true;
		}
	}

	if (!continueSend)
	{
		return;
	}

	// do we have data to send?
	if (m_connectionState != CS_ACTIVE)
	{
		return;
	}

	// metrics
	NetPacketMetrics metrics;

	// build a nice packet
	NetBuffer msg(24000);

	msg.Write(m_lastReceivedReliableCommand);

	if (m_serverProtocol >= 2)
	{
		msg.Write(m_lastFrameNumber);
	}

	RoutingPacket packet;

	while (m_outgoingPackets.try_pop(packet))
	{
		msg.Write(0xE938445B); // msgRoute
		msg.Write(packet.netID);
		msg.Write<uint16_t>(packet.payload.size());

		//trace("sending msgRoute to %d len %d\n", packet.netID, packet.payload.size());

		msg.Write(packet.payload.c_str(), packet.payload.size());

		metrics.AddElementSize(NET_PACKET_SUB_ROUTED_MESSAGES, packet.payload.size() + 2 + 2 + 4);
	}

	// send pending reliable commands
	for (auto& command : m_outReliableCommands)
	{
		msg.Write(command.type);

		if (command.command.size() > UINT16_MAX)
		{
			msg.Write(command.id | 0x80000000);

			msg.Write<uint32_t>(command.command.size());

			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4);
		}
		else
		{
			msg.Write(command.id);

			msg.Write<uint16_t>(command.command.size());

			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 2);
		}

		msg.Write(command.command.c_str(), command.command.size());

		metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, command.command.size() + 8);
	}

	// FIXME: REPLACE HARDCODED STUFF
/*	if (*(BYTE*)0x18A82FD) // is server running
	{
		msg.Write(0xB3EA30DE); // msgIHost
		msg.Write(m_serverBase);
	}*/

	OnBuildMessage(msg);

	msg.Write(0xCA569E63); // msgEnd

	m_netChannel.Send(msg);

	m_lastSend = timeGetTime();

	if (m_metricSink.GetRef())
	{
		m_metricSink->OnOutgoingPacket(metrics);
	}
}

void NetLibrary::SendReliableCommand(const char* type, const char* buffer, size_t length)
{
	uint32_t unacknowledged = m_outReliableSequence - m_outReliableAcknowledged;

	if (unacknowledged > MAX_RELIABLE_COMMANDS)
	{
		GlobalError("Reliable client command overflow.");
	}

	m_outReliableSequence++;

	OutReliableCommand cmd;
	cmd.type = HashRageString(type);
	cmd.id = m_outReliableSequence;
	cmd.command = std::string(buffer, length);

	m_outReliableCommands.push_back(cmd);
}

static std::string g_disconnectReason;

static std::mutex g_netFrameMutex;

void NetLibrary::PreProcessNativeNet()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	ProcessPackets();

	g_netFrameMutex.unlock();
}

void NetLibrary::PostProcessNativeNet()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	ProcessSend();

	g_netFrameMutex.unlock();
}

inline uint64_t GetGUID()
{
	auto steamComponent = GetSteam();

	if (steamComponent)
	{
		IClientEngine* steamClient = steamComponent->GetPrivateClient();

		InterfaceMapper steamUser(steamClient->GetIClientUser(steamComponent->GetHSteamUser(), steamComponent->GetHSteamPipe(), "CLIENTUSER_INTERFACE_VERSION001"));

		if (steamUser.IsValid())
		{
			uint64_t steamID;
			steamUser.Invoke<void>("GetSteamID", &steamID);

			return steamID;
		}
	}

	return (uint64_t)(0x210000100000000 | m_tempGuid);
}

void NetLibrary::RunFrame()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	if (m_connectionState != m_lastConnectionState)
	{
		OnStateChanged(m_connectionState, m_lastConnectionState);

		m_lastConnectionState = m_connectionState;
	}

	ProcessPackets();

	ProcessSend();

	switch (m_connectionState)
	{
		case CS_INITRECEIVED:
			// change connection state to CS_DOWNLOADING
			m_connectionState = CS_DOWNLOADING;

			// trigger task event
			OnConnectionProgress("Downloading content", 0, 1);
			OnInitReceived(m_currentServer);

			break;

		case CS_DOWNLOADCOMPLETE:
			m_connectionState = CS_FETCHING;
			m_lastConnect = 0;
			m_connectAttempts = 0;

			OnConnectionProgress("Downloading completed", 1, 1);

			break;

		case CS_FETCHING:
			if ((GetTickCount() - m_lastConnect) > 5000)
			{
				SendOutOfBand(m_currentServer, "getinfo xyz");

				m_lastConnect = GetTickCount();

				m_connectAttempts++;

				// advertise status
				auto specStatus = (m_connectAttempts > 1) ? fmt::sprintf(" (attempt %d)", m_connectAttempts) : "";

				OnConnectionProgress(fmt::sprintf("Fetching info from server...%s", specStatus), 1, 1);
			}

			if (m_connectAttempts > 3)
			{
				g_disconnectReason = "Fetching info timed out.";
				FinalizeDisconnect();

				OnConnectionTimedOut();

				GlobalError("Failed to getinfo server after 3 attempts.");
			}
			break;

		case CS_CONNECTING:
			if ((GetTickCount() - m_lastConnect) > 5000)
			{
				SendOutOfBand(m_currentServer, "connect token=%s&guid=%llu", m_token.c_str(), (uint64_t)GetGUID());

				m_lastConnect = GetTickCount();

				m_connectAttempts++;

				// advertise status
				auto specStatus = (m_connectAttempts > 1) ? fmt::sprintf(" (attempt %d)", m_connectAttempts) : "";

				OnConnectionProgress(fmt::sprintf("Connecting to server...%s", specStatus), 1, 1);
			}

			if (m_connectAttempts > 3)
			{
				g_disconnectReason = "Connection timed out.";
				FinalizeDisconnect();

				OnConnectionTimedOut();

				GlobalError("Failed to connect to server after 3 attempts.");
			}

			break;

		case CS_ACTIVE:
			if ((GetTickCount() - m_lastReceivedAt) > 15000)
			{
				g_disconnectReason = "Connection timed out.";
				FinalizeDisconnect();

				OnConnectionTimedOut();

				GlobalError("Server connection timed out after 15 seconds.");
			}

			break;
	}

	g_netFrameMutex.unlock();
}

void NetLibrary::Death()
{
	g_netFrameMutex.unlock();
}

void NetLibrary::Resurrection()
{
	g_netFrameMutex.lock();
}

static void tohex(unsigned char* in, size_t insz, char* out, size_t outsz)
{
    unsigned char* pin = in;
    const char* hex = "0123456789ABCDEF";
    char* pout = out;
    for (; pin < in + insz; pout += 2, pin++)
    {
        pout[0] = hex[(*pin >> 4) & 0xF];
        pout[1] = hex[*pin & 0xF];
        if (pout + 3 - out > outsz)
        {
            break;
        }
    }
    pout[0] = 0;
}

void NetLibrary::ConnectToServer(const char* hostname, uint16_t port)
{
	if (m_connectionState != CS_IDLE)
	{
		Disconnect("Connecting to another server.");
		FinalizeDisconnect();
	}

	// late-initialize error state in ICoreGameInit
	// this happens here so it only tries capturing if connection was attempted
	static struct ErrorState 
	{
		ErrorState(NetLibrary* lib)
		{
			Instance<ICoreGameInit>::Get()->OnTriggerError.Connect([=] (const std::string& errorMessage)
			{
				if (lib->m_connectionState != CS_ACTIVE)
				{
					lib->OnConnectionError(errorMessage.c_str());

					lib->m_connectionState = CS_IDLE;

					return false;
				}
				else if (lib->m_connectionState != CS_IDLE)
				{
					auto nlPos = errorMessage.find_first_of('\n');

					if (nlPos == std::string::npos || nlPos > 100)
					{
						nlPos = 100;
					}

					lib->Disconnect(errorMessage.substr(0, nlPos).c_str());
					lib->FinalizeDisconnect();
				}

				return true;
			});
		}
	} es(this);

	m_connectionState = CS_INITING;
	m_currentServer = NetAddress(hostname, port);

	m_outReliableAcknowledged = 0;
	m_outReliableSequence = 0;
	m_outSequence = 0;
	m_lastReceivedReliableCommand = 0;
	m_outReliableCommands.clear();

	m_lastFrameNumber = 0;

	wchar_t wideHostname[256];
	mbstowcs(wideHostname, hostname, _countof(wideHostname) - 1);

	wideHostname[255] = L'\0';

	fwWString wideHostnameStr = wideHostname;

	static fwMap<fwString, fwString> postMap;
	postMap["method"] = "initConnect";
	postMap["name"] = GetPlayerName();
	postMap["protocol"] = va("%d", NETWORK_PROTOCOL);

	uint16_t capturePort = port;

	auto steamComponent = GetSteam();

	if (steamComponent)
	{
		uint32_t ticketLength;
		uint8_t ticketBuffer[4096];

		IClientEngine* steamClient = steamComponent->GetPrivateClient();

		InterfaceMapper steamUser(steamClient->GetIClientUser(steamComponent->GetHSteamUser(), steamComponent->GetHSteamPipe(), "CLIENTUSER_INTERFACE_VERSION001"));

		if (steamUser.IsValid())
		{
			steamUser.Invoke<int>("GetAuthSessionTicket", ticketBuffer, (int)sizeof(ticketBuffer), &ticketLength);

			// encode the ticket buffer
            char outHex[16384];
            tohex(ticketBuffer, ticketLength, outHex, sizeof(outHex));

			postMap["authTicket"] = outHex;
		}
	}

	postMap["guid"] = va("%lld", GetGUID());

	static fwAction<bool, const char*, size_t> handleAuthResult;
	handleAuthResult = [=] (bool result, const char* connDataStr, size_t size) mutable
	{
		std::string connData(connDataStr, size);

		if (!result)
		{
			// TODO: add UI output
			m_connectionState = CS_IDLE;

			//nui::ExecuteRootScript("citFrames[\"mpMenu\"].contentWindow.postMessage({ type: 'connectFailed', message: 'General handshake failure.' }, '*');");
			OnConnectionError(va("Failed handshake to server %s:%d%s%s.", m_currentServer.GetAddress(), m_currentServer.GetPort(), connData.length() > 0 ? " - " : "", connData));

			return;
		}

		try
		{
			auto node = YAML::Load(connData);

			// ha-ha, you need to authenticate first!
			/*
			if (node["authID"].IsDefined())
			{
				if (postMap.find("authTicket") == postMap.end())
				{
					std::vector<uint8_t> authTicket = user->GetUserTicket(node["authID"].as<uint64_t>());

					size_t ticketLen;
					char* ticketEncoded = base64_encode(&authTicket[0], authTicket.size(), &ticketLen);

					postMap["authTicket"] = fwString(ticketEncoded, ticketLen);

					free(ticketEncoded);

					m_httpClient->DoPostRequest(wideHostnameStr, capturePort, L"/client", postMap, handleAuthResult);
				}
				else
				{
					postMap.erase("authTicket");

					GlobalError("you're so screwed, the server still asked for an auth ticket even though we gave them one");
				}

				return;
			}

			postMap.erase("authTicket");*/

			if (node["error"].IsDefined())
			{
				// FIXME: single quotes
				//nui::ExecuteRootScript(va("citFrames[\"mpMenu\"].contentWindow.postMessage({ type: 'connectFailed', message: '%s' }, '*');", node["error"].as<std::string>().c_str()));
				OnConnectionError(node["error"].as<std::string>().c_str());

				m_connectionState = CS_IDLE;

				return;
			}

			if (!node["sH"].IsDefined())
			{
				// Server did not send a scripts setting: old server or rival project
				OnConnectionError("Legacy servers are incompatible with this version of FiveM. Update the server to the latest files from fivem.net");
				m_connectionState = CS_IDLE;
				return;
			}
			else Instance<ICoreGameInit>::Get()->ShAllowed = node["sH"].as<bool>(true);

			Instance<ICoreGameInit>::Get()->EnhancedHostSupport = (node["enhancedHostSupport"].IsDefined() && node["enhancedHostSupport"].as<bool>(false));

			m_token = node["token"].as<std::string>();

			m_serverProtocol = node["protocol"].as<uint32_t>();

			auto steam = GetSteam();

			if (steam)
			{
				steam->SetConnectValue(fmt::sprintf("+connect %s:%d", m_currentServer.GetAddress(), m_currentServer.GetPort()));
			}

			m_connectionState = CS_INITRECEIVED;
		}
		catch (YAML::Exception&)
		{
			m_connectionState = CS_IDLE;
		}
	};

	m_httpClient->DoPostRequest(wideHostname, port, L"/client", postMap, handleAuthResult);
}

void NetLibrary::Disconnect(const char* reason)
{
	g_disconnectReason = reason;

	OnAttemptDisconnect(reason);
	//GameInit::KillNetwork((const wchar_t*)1);
}

void NetLibrary::FinalizeDisconnect()
{
	if (m_connectionState == CS_CONNECTING || m_connectionState == CS_ACTIVE)
	{
		SendReliableCommand("msgIQuit", g_disconnectReason.c_str(), g_disconnectReason.length() + 1);

		m_lastSend = 0;
		ProcessSend();

		m_lastSend = 0;
		ProcessSend();

		OnFinalizeDisconnect(m_currentServer);
		//GameFlags::ResetFlags();

		//TheResources.CleanUp();
		//TheDownloads.ReleaseLastServer();

		m_connectionState = CS_IDLE;
		m_currentServer = NetAddress();

		//GameInit::MurderGame();
	}
}

void NetLibrary::CreateResources()
{
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		GlobalError("WSAStartup did not succeed.");
	}

	// create IPv4 socket
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (m_socket == INVALID_SOCKET)
	{
		GlobalError("only one sock in pair");
	}

	// explicit bind
	sockaddr_in localAddr = { 0 };
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = ADDR_ANY;
	localAddr.sin_port = 0;

	bind(m_socket, (sockaddr*)&localAddr, sizeof(localAddr));

	// non-blocking
	u_long arg = true;
	ioctlsocket(m_socket, FIONBIO, &arg);

	// create IPv6 socket
	m_socket6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

	if (m_socket6 != INVALID_SOCKET)
	{
		// bind the socket
		sockaddr_in6 ip6Addr = { 0 };
		ip6Addr.sin6_family = AF_INET6;
		ip6Addr.sin6_addr = in6addr_any;
		ip6Addr.sin6_port = 0;

		bind(m_socket6, (sockaddr*)&ip6Addr, sizeof(ip6Addr));

		// make non-blocking
		ioctlsocket(m_socket6, FIONBIO, &arg);
	}

	m_httpClient = new HttpClient();
	//m_httpClient = new HttpClient();

	// TEMPTEMP
	/*uint8_t out[1024];
	uint8_t in[] = { 0x19, 0x00, 0xF7, 0x03, 0xC7, 0x40, 0x00, 0x02, 0x00, 0x01, 0xB4, 0x8D, 0xFD, 0x94, 0x8D, 0xAD, 0x03, 0xC5, 0xC0, 0xE4, 0x00, 0xB0, 0xF0, 0xDA, 0x30, 0xDA, 0x4D, 0x03, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xB0, 0x53, 0xF0, 0x54, 0x0D };

	__asm
	{
		push edx
		mov ecx, 785B70h
		mov edx, 1bh

		lea eax, in
		push eax
		push 1021

		lea eax, out

		call ecx

		add esp, 8

		pop edx
	}*/
}

void NetLibrary::SendOutOfBand(NetAddress& address, const char* format, ...)
{
	static char buffer[32768];

	*(int*)buffer = -1;

	va_list ap;
	va_start(ap, format);
	int length = _vsnprintf(&buffer[4], 32764, format, ap);
	va_end(ap);

	if (length >= 32764)
	{
		GlobalError("Attempted to overrun string in call to SendOutOfBand()!");
	}

	buffer[32767] = '\0';

	SendData(address, buffer, strlen(buffer));
}

const char* NetLibrary::GetPlayerName()
{
	/*
	ProfileManager* profileManager = Instance<ProfileManager>::Get();
	fwRefContainer<Profile> profile = profileManager->GetPrimaryProfile();*/
	if (!m_playerName.empty()) return m_playerName.c_str();

	auto steamComponent = GetSteam();

	if (steamComponent)
	{
		IClientEngine* steamClient = steamComponent->GetPrivateClient();

		if (steamClient)
		{
			InterfaceMapper steamFriends(steamClient->GetIClientFriends(steamComponent->GetHSteamUser(), steamComponent->GetHSteamPipe(), "CLIENTFRIENDS_INTERFACE_VERSION001"));

			if (steamFriends.IsValid())
			{
				// TODO: name changing
				static std::string personaName = steamFriends.Invoke<const char*>("GetPersonaName");

				return personaName.c_str();
			}
		}
	}

	const char* returnName = nullptr;
	/*
	if (profile.GetRef())
	{
		returnName = profile->GetDisplayName();
	}
	else
	{
		static char computerName[64];
		DWORD nameSize = sizeof(computerName);
		GetComputerNameA(computerName, &nameSize);

		returnName = computerName;
	}*/
	returnName = getenv("USERNAME");
	if (returnName == nullptr || !returnName[0]) {
		static char computerName[64];
		DWORD nameSize = sizeof(computerName);
		GetComputerNameA(computerName, &nameSize);
		returnName = computerName;
	}
	if (returnName == nullptr || !returnName[0]) {
		returnName = "Unknown Solderer";
	}
	return returnName;
}

void NetLibrary::SetPlayerName(const char* name)
{
	m_playerName = name;
}

void NetLibrary::SendData(NetAddress& address, const char* data, size_t length)
{
	sockaddr_storage addr;
	int addrLen;
	address.GetSockAddr(&addr, &addrLen);

	if (addr.ss_family == AF_INET)
	{
		sendto(m_socket, data, length, 0, (sockaddr*)&addr, addrLen);
	}
	else if (addr.ss_family == AF_INET6)
	{
		sendto(m_socket6, data, length, 0, (sockaddr*)&addr, addrLen);
	}
}

void NetLibrary::AddReliableHandler(const char* type, ReliableHandlerType function)
{
	uint32_t hash = HashRageString(type);

	m_reliableHandlers.insert(std::make_pair(hash, function));
}

void NetLibrary::DownloadsComplete()
{
	if (m_connectionState == CS_DOWNLOADING)
	{
		m_connectionState = CS_DOWNLOADCOMPLETE;
	}
}

bool NetLibrary::ProcessPreGameTick()
{
	if (m_connectionState != CS_ACTIVE && m_connectionState != CS_CONNECTED && m_connectionState != CS_IDLE)
	{
		RunFrame();

		return false;
	}

	return true;
}

void NetLibrary::SendNetEvent(const std::string& eventName, const std::string& jsonString, int i)
{
	const char* cmdType = "msgNetEvent";

	if (i == -1)
	{
		i = UINT16_MAX;
	}
	else if (i == -2)
	{
		cmdType = "msgServerEvent";
	}

	size_t eventNameLength = eventName.length();

	NetBuffer buffer(100000);

	if (i >= 0)
	{
		buffer.Write<uint16_t>(i);
	}

	buffer.Write<uint16_t>(eventNameLength + 1);
	buffer.Write(eventName.c_str(), eventNameLength + 1);

	buffer.Write(jsonString.c_str(), jsonString.size());
	
	SendReliableCommand(cmdType, buffer.GetBuffer(), buffer.GetCurLength());
}

/*void NetLibrary::AddReliableHandler(const char* type, ReliableHandlerType function)
{
	netLibrary.AddReliableHandlerImpl(type, function);
}*/

NetLibrary::NetLibrary()
	: m_serverNetID(0), m_serverBase(0), m_hostBase(0), m_hostNetID(0), m_connectionState(CS_IDLE), m_lastConnectionState(CS_IDLE),
	  m_lastConnect(0), m_lastSend(0), m_outSequence(0), m_lastReceivedReliableCommand(0), m_outReliableAcknowledged(0), m_outReliableSequence(0),
	  m_lastReceivedAt(0)

{
	m_receiveEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

__declspec(dllexport) fwEvent<NetLibrary*> NetLibrary::OnNetLibraryCreate;

NetLibrary* NetLibrary::Create()
{
	auto lib = new NetLibrary();

	lib->CreateResources();

	lib->AddReliableHandler("msgIHost", [=] (const char* buf, size_t len)
	{
		NetBuffer buffer(buf, len);

		uint16_t hostNetID = buffer.Read<uint16_t>();
		uint32_t hostBase = buffer.Read<uint32_t>();

		lib->SetHost(hostNetID, hostBase);
	});

	OnNetLibraryCreate(lib);

	return lib;
}