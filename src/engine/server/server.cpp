/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <base/math.h>
#include <base/system.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/map.h>
#include <engine/masterserver.h>
#include <engine/server.h>
#include <engine/storage.h>

#include <engine/shared/compression.h>
#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/shared/demo.h>
#include <engine/shared/econ.h>
#include <engine/shared/filecollection.h>
#include <engine/shared/mapchecker.h>
#include <engine/shared/netban.h>
#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>
#include <engine/shared/snapshot.h>

#include <mastersrv/mastersrv.h>

#include "register.h"
#include "server.h"

#include "curl/curl.h"
#include <cstring>

#if defined(CONF_FAMILY_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#endif

#include <engine/server/databases/connection_pool.h>
#include <engine/storage.h>

#define IO_MAX_PATH_LENGTH 512

static const char *StrLtrim(const char *pStr)
{
	while(*pStr && *pStr >= 0 && *pStr <= 32)
		pStr++;
	return pStr;
}

static void StrRtrim(char *pStr)
{
	int i = str_length(pStr);
	while(i >= 0)
	{
		if(pStr[i] < 0 || pStr[i] > 32)
			break;
		pStr[i] = 0;
		i--;
	}
}

sMap::~sMap() {
	if (m_pCurrentMapData)
		mem_free(m_pCurrentMapData);
	if (m_pMap) {
		delete m_pMap;
	}
}

CSnapIDPool::CSnapIDPool()
{
	Reset();
}

void CSnapIDPool::Reset()
{
	for(int i = 0; i < MAX_IDS; i++)
	{
		m_aIDs[i].m_Next = i+1;
		m_aIDs[i].m_State = 0;
	}

	m_aIDs[MAX_IDS-1].m_Next = -1;
	m_FirstFree = 0;
	m_FirstTimed = -1;
	m_LastTimed = -1;
	m_Usage = 0;
	m_InUsage = 0;
}


void CSnapIDPool::RemoveFirstTimeout()
{
	int NextTimed = m_aIDs[m_FirstTimed].m_Next;

	// add it to the free list
	m_aIDs[m_FirstTimed].m_Next = m_FirstFree;
	m_aIDs[m_FirstTimed].m_State = 0;
	m_FirstFree = m_FirstTimed;

	// remove it from the timed list
	m_FirstTimed = NextTimed;
	if(m_FirstTimed == -1)
		m_LastTimed = -1;

	m_Usage--;
}

int CSnapIDPool::NewID()
{
	int64 Now = time_get();

	// process timed ids
	while(m_FirstTimed != -1 && m_aIDs[m_FirstTimed].m_Timeout < Now)
		RemoveFirstTimeout();

	int ID = m_FirstFree;
	dbg_assert(ID != -1, "id error");
	if(ID == -1)
		return ID;
	m_FirstFree = m_aIDs[m_FirstFree].m_Next;
	m_aIDs[ID].m_State = 1;
	m_Usage++;
	m_InUsage++;
	return ID;
}

void CSnapIDPool::TimeoutIDs()
{
	// process timed ids
	while(m_FirstTimed != -1)
		RemoveFirstTimeout();
}

void CSnapIDPool::FreeID(int ID)
{
	if(ID < 0)
		return;
	dbg_assert(m_aIDs[ID].m_State == 1, "id is not alloced");

	m_InUsage--;
	m_aIDs[ID].m_State = 2;
	m_aIDs[ID].m_Timeout = time_get()+time_freq()*5;
	m_aIDs[ID].m_Next = -1;

	if(m_LastTimed != -1)
	{
		m_aIDs[m_LastTimed].m_Next = ID;
		m_LastTimed = ID;
	}
	else
	{
		m_FirstTimed = ID;
		m_LastTimed = ID;
	}
}


void CServerBan::InitServerBan(IConsole *pConsole, IStorage *pStorage, CServer* pServer)
{
	CNetBan::Init(pConsole, pStorage);

	m_pServer = pServer;

	// overwrites base command, todo: improve this
	Console()->Register("ban", "s?ir", CFGFLAG_SERVER|CFGFLAG_STORE, ConBanExt, this, "Ban player with ip/client id for x minutes for any reason");
}

template<class T>
int CServerBan::BanExt(T *pBanPool, const typename T::CDataType *pData, int Seconds, const char *pReason)
{
	// validate address
	if(Server()->m_RconClientID >= 0 && Server()->m_RconClientID < MAX_CLIENTS &&
		Server()->m_aClients[Server()->m_RconClientID].m_State != CServer::CClient::STATE_EMPTY)
	{
		if(NetMatch(pData, Server()->m_NetServer.ClientAddr(Server()->m_RconClientID)))
		{
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (you can't ban yourself)");
			return -1;
		}

		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(i == Server()->m_RconClientID || Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->m_aClients[i].m_Authed >= Server()->m_RconAuthLevel && NetMatch(pData, Server()->m_NetServer.ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}
	else if(Server()->m_RconClientID == IServer::RCON_CID_VOTE)
	{
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(Server()->m_aClients[i].m_Authed != CServer::AUTHED_NO && NetMatch(pData, Server()->m_NetServer.ClientAddr(i)))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (command denied)");
				return -1;
			}
		}
	}

	int Result = Ban(pBanPool, pData, Seconds, pReason);
	if(Result != 0)
		return Result;

	// drop banned clients
	typename T::CDataType Data = *pData;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
			continue;

		if(NetMatch(&Data, Server()->m_NetServer.ClientAddr(i)))
		{
			CNetHash NetHash(&Data);
			char aBuf[256];
			MakeBanInfo(pBanPool->Find(&Data, &NetHash), aBuf, sizeof(aBuf), MSGTYPE_PLAYER);
			Server()->m_NetServer.Drop(i, aBuf);
		}
	}

	return Result;
}

int CServerBan::BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason)
{
	return BanExt(&m_BanAddrPool, pAddr, Seconds, pReason);
}

int CServerBan::BanAddr(const NETADDR *pAddr, int Seconds, const char *pReason, bool force)
{
	if(force){
		int ret = Ban(&m_BanAddrPool, pAddr, Seconds, pReason);
		if(ret != 0) return ret;
		
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(Server()->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY)
				continue;

			if(NetMatch(pAddr, Server()->m_NetServer.ClientAddr(i)))
			{
				Server()->m_NetServer.Drop(i, pReason);
			}
		}
		return ret;
	} else return BanExt(&m_BanAddrPool, pAddr, Seconds, pReason);
}


int CServerBan::BanRange(const CNetRange *pRange, int Seconds, const char *pReason)
{
	if(pRange->IsValid())
		return BanExt(&m_BanRangePool, pRange, Seconds, pReason);

	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban failed (invalid range)");
	return -1;
}

void CServerBan::ConBanExt(IConsole::IResult *pResult, void *pUser)
{
	CServerBan *pThis = static_cast<CServerBan *>(pUser);

	const char *pStr = pResult->GetString(0);
	int Minutes = pResult->NumArguments()>1 ? clamp(pResult->GetInteger(1), 0, 44640) : 30;
	const char *pReason = pResult->NumArguments()>2 ? pResult->GetString(2) : "No reason given";

	if(StrAllnum(pStr))
	{
		int ClientID = str_toint(pStr);
		if(ClientID < 0 || ClientID >= MAX_CLIENTS || pThis->Server()->m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "net_ban", "ban error (invalid client id)");
		else
			pThis->BanAddr(pThis->Server()->m_NetServer.ClientAddr(ClientID), Minutes*60, pReason);
	}
	else
		ConBan(pResult, pUser);
}


void CServer::CClient::Reset()
{
	// reset input
	for(int i = 0; i < 200; i++)
		m_aInputs[i].m_GameTick = -1;
	m_CurrentInput = 0;
	mem_zero(&m_LatestInput, sizeof(m_LatestInput));

	m_Snapshots.PurgeAll();
	m_LastAckedSnapshot = -1;
	m_LastInputTick = -1;
	m_SnapRate = CClient::SNAPRATE_INIT;
	m_Score = 0;
	m_Version = -1;
	m_UnknownFlags = 0;
}

CServer::CServer() : m_DemoRecorder(&m_SnapshotDelta)
{
	m_TickSpeed = SERVER_TICK_SPEED;

	m_pGames = new sGame;
	m_pMaps = NULL;

	m_CurrentGameTick = 0;
	m_RunServer = 1;
	m_StopServerWhenEmpty = 0;

	m_pCurrentMapData = 0;
	m_CurrentMapSize = 0;

	m_MapReload = 0;

	m_RconClientID = IServer::RCON_CID_SERV;
	m_RconAuthLevel = AUTHED_ADMIN;
	
	m_PlayerCount = 0;

	Init();
}


int CServer::TrySetClientName(int ClientID, const char *pName)
{
	char aTrimmedName[64];

	// trim the name
	str_copy(aTrimmedName, StrLtrim(pName), sizeof(aTrimmedName));
	StrRtrim(aTrimmedName);

	// check for empty names
	if(!aTrimmedName[0])
		return -1;

	// check if new and old name are the same
	if(m_aClients[ClientID].m_aName[0] && str_comp(m_aClients[ClientID].m_aName, aTrimmedName) == 0)
		return 0;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "'%s' -> '%s'", pName, aTrimmedName);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);
	pName = aTrimmedName;

	// make sure that two clients doesn't have the same name
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(i != ClientID && m_aClients[i].m_State >= CClient::STATE_READY)
		{
			if(str_comp(pName, m_aClients[i].m_aName) == 0)
				return -1;
		}

	// set the client name
	str_copy(m_aClients[ClientID].m_aName, pName, MAX_NAME_LENGTH);
	return 0;
}



void CServer::SetClientName(int ClientID, const char *pName)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	if(!pName)
		return;

	char aCleanName[MAX_NAME_LENGTH];
	str_copy(aCleanName, pName, sizeof(aCleanName));

	//allow all names, all utf8
	/*// clear name
	for(char *p = aCleanName; *p; ++p)
	{
		if(*p < 32)
			*p = ' ';
	}*/

	if(TrySetClientName(ClientID, aCleanName))
	{
		// auto rename
		for(int i = 1;; i++)
		{
			char aNameTry[MAX_NAME_LENGTH];
			str_format(aNameTry, sizeof(aCleanName), "(%d)%s", i, aCleanName);
			if(TrySetClientName(ClientID, aNameTry) == 0)
				break;
		}
	}
}

void CServer::SetClientClan(int ClientID, const char *pClan)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY || !pClan)
		return;

	str_copy(m_aClients[ClientID].m_aClan, pClan, MAX_CLAN_LENGTH);
}

void CServer::SetClientCountry(int ClientID, int Country)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;

	m_aClients[ClientID].m_Country = Country;
}

void CServer::SetClientScore(int ClientID, int Score)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;
	m_aClients[ClientID].m_Score = Score;
}

void CServer::SetClientVersion(int ClientID, int Version)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;
	m_aClients[ClientID].m_Version = Version;
}

void CServer::SetClientUnknownFlags(int ClientID, int UnknownFlags)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State < CClient::STATE_READY)
		return;
	m_aClients[ClientID].m_UnknownFlags = UnknownFlags;
}

void CServer::Kick(int ClientID, const char *pReason)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CClient::STATE_EMPTY)
	{
		//Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "invalid client id to kick");
		return;
	}
	else if(m_RconClientID == ClientID)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "you can't kick yourself");
 		return;
	}
	else if(m_aClients[ClientID].m_Authed > m_RconAuthLevel)
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "kick command denied");
 		return;
	}

	m_NetServer.Drop(ClientID, pReason);
}

void CServer::KickForce(int ClientID, const char *pReason)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CClient::STATE_EMPTY)
	{
		//Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", "invalid client id to kick");
		return;
	}

	m_NetServer.Drop(ClientID, pReason);
}

/*int CServer::Tick()
{
	return m_CurrentGameTick;
}*/

int64 CServer::TickStartTime(int Tick)
{
	return m_GameStartTime + (time_freq()*Tick)/SERVER_TICK_SPEED;
}

/*int CServer::TickSpeed()
{
	return SERVER_TICK_SPEED;
}*/

int CServer::Init()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		m_aClients[i].m_State = CClient::STATE_EMPTY;
		m_aClients[i].m_aName[0] = 0;
		m_aClients[i].m_aClan[0] = 0;
		m_aClients[i].m_Country = -1;
		m_aClients[i].m_Snapshots.Init();
		m_aClients[i].m_Traffic = 0;
		m_aClients[i].m_TrafficSince = 0;
		m_aClients[i].m_PreferedTeam = -2;
		m_aClients[i].m_uiGameID = GAME_ID_INVALID;
	}

	m_CurrentGameTick = 0;

	return 0;
}

void CServer::SetRconCID(int ClientID)
{
	m_RconClientID = ClientID;
}

bool CServer::IsAuthed(int ClientID)
{
	return m_aClients[ClientID].m_Authed;
}

int CServer::GetClientInfo(int ClientID, CClientInfo *pInfo)
{
	dbg_assert(ClientID >= 0 && ClientID < MAX_CLIENTS, "client_id is not valid");
	dbg_assert(pInfo != 0, "info can not be null");

	if(m_aClients[ClientID].m_State == CClient::STATE_INGAME)
	{
		pInfo->m_pName = m_aClients[ClientID].m_aName;
		pInfo->m_Latency = m_aClients[ClientID].m_Latency;
		return 1;
	}
	return 0;
}

void CServer::GetClientAddr(int ClientID, char *pAddrStr, int Size)
{
	if(ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_State == CClient::STATE_INGAME)
		net_addr_str(m_NetServer.ClientAddr(ClientID), pAddrStr, Size, false);
}


const char *CServer::ClientName(int ClientID, bool ForceGet)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return "(invalid)";
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME || ForceGet)
		return m_aClients[ClientID].m_aName;
	else
		return "(connecting)";

}

const char *CServer::ClientClan(int ClientID, bool ForceGet)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return "";
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME || ForceGet)
		return m_aClients[ClientID].m_aClan;
	else
		return "";
}

int CServer::ClientCountry(int ClientID, bool ForceGet)
{
	if(ClientID < 0 || ClientID >= MAX_CLIENTS || m_aClients[ClientID].m_State == CServer::CClient::STATE_EMPTY)
		return -1;
	if(m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME || ForceGet)
		return m_aClients[ClientID].m_Country;
	else
		return -1;
}

bool CServer::ClientIngame(int ClientID)
{
	return ClientID >= 0 && ClientID < MAX_CLIENTS && m_aClients[ClientID].m_State == CServer::CClient::STATE_INGAME;
}

int CServer::MaxClients() const
{
	return m_NetServer.MaxClients();
}

int CServer::SendMsg(CMsgPacker *pMsg, int Flags, int ClientID)
{
	return SendMsgEx(pMsg, Flags, ClientID, false);
}

int CServer::SendMsgEx(CMsgPacker *pMsg, int Flags, int ClientID, bool System)
{
	CNetChunk Packet;
	if(!pMsg)
		return -1;

	mem_zero(&Packet, sizeof(CNetChunk));

	Packet.m_ClientID = ClientID;
	Packet.m_pData = pMsg->Data();
	Packet.m_DataSize = pMsg->Size();

	// HACK: modify the message id in the packet and store the system flag
	*((unsigned char*)Packet.m_pData) <<= 1;
	if(System)
		*((unsigned char*)Packet.m_pData) |= 1;

	if(Flags&MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags&MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;

	// write message to demo recorder
	if(!(Flags&MSGFLAG_NORECORD))
		m_DemoRecorder.RecordMessage(pMsg->Data(), pMsg->Size());

	if(!(Flags&MSGFLAG_NOSEND))
	{
		if(ClientID == -1)
		{
			// broadcast
			int i;
			for(i = 0; i < MAX_CLIENTS; i++)
				if(m_aClients[i].m_State == CClient::STATE_INGAME)
				{
					Packet.m_ClientID = i;
					m_NetServer.Send(&Packet);
				}
		}
		else
			m_NetServer.Send(&Packet);
	}
	return 0;
}

void CServer::DoSnapshot()
{
	sGame* p = m_pGames;
	while(p != NULL){	
		p->GameServer()->OnPreSnap();
		p = p->m_pNext;
	}

	// create snapshot for demo recording
	if(m_DemoRecorder.IsRecording())
	{
		char aData[CSnapshot::MAX_SIZE];
		int SnapshotSize;

		// build snap and possibly add some messages
		m_SnapshotBuilder.Init();
		GameServer()->OnSnap(-1);
		SnapshotSize = m_SnapshotBuilder.Finish(aData);

		// write snapshot
		m_DemoRecorder.RecordSnapshot(Tick(), aData, SnapshotSize);
	}

	// create snapshots for all clients
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		// client must be ingame to recive snapshots
		if(m_aClients[i].m_State != CClient::STATE_INGAME)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_RECOVER && (Tick()%SERVER_TICK_SPEED) != 0)
			continue;

		// this client is trying to recover, don't spam snapshots
		if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_INIT && (Tick()%(10*SERVER_TICK_SPEED/50)) != 0)
			continue;

		{
			char aData[CSnapshot::MAX_SIZE];
			CSnapshot *pData = (CSnapshot*)aData;	// Fix compiler warning for strict-aliasing
			char aDeltaData[CSnapshot::MAX_SIZE];
			char aCompData[CSnapshot::MAX_SIZE];
			int SnapshotSize;
			int Crc;
			static CSnapshot EmptySnap;
			CSnapshot *pDeltashot = &EmptySnap;
			int DeltashotSize;
			int DeltaTick = -1;
			int DeltaSize;

			m_SnapshotBuilder.Init();

			sGame* p = GetGame(m_aClients[i].m_uiGameID);
			if(p != NULL) p->GameServer()->OnSnap(i);

			// finish snapshot
			SnapshotSize = m_SnapshotBuilder.Finish(pData);
			Crc = pData->Crc();

			// remove old snapshos
			// keep 3 seconds worth of snapshots
			m_aClients[i].m_Snapshots.PurgeUntil(m_CurrentGameTick-SERVER_TICK_SPEED*3);

			// save it the snapshot
			m_aClients[i].m_Snapshots.Add(m_CurrentGameTick, time_get(), SnapshotSize, pData, 0);

			// find snapshot that we can preform delta against
			EmptySnap.Clear();

			{
				DeltashotSize = m_aClients[i].m_Snapshots.Get(m_aClients[i].m_LastAckedSnapshot, 0, &pDeltashot, 0);
				if(DeltashotSize >= 0)
					DeltaTick = m_aClients[i].m_LastAckedSnapshot;
				else
				{
					// no acked package found, force client to recover rate
					if(m_aClients[i].m_SnapRate == CClient::SNAPRATE_FULL)
						m_aClients[i].m_SnapRate = CClient::SNAPRATE_RECOVER;
				}
			}

			// create delta
			DeltaSize = m_SnapshotDelta.CreateDelta(pDeltashot, pData, aDeltaData);

			if(DeltaSize)
			{
				// compress it
				int SnapshotSize;
				const int MaxSize = MAX_SNAPSHOT_PACKSIZE;
				int NumPackets;

				SnapshotSize = CVariableInt::Compress(aDeltaData, DeltaSize, aCompData);
				NumPackets = (SnapshotSize+MaxSize-1)/MaxSize;

				for(int n = 0, Left = SnapshotSize; Left; n++)
				{
					int Chunk = Left < MaxSize ? Left : MaxSize;
					Left -= Chunk;

					if(NumPackets == 1)
					{
						CMsgPacker Msg(NETMSG_SNAPSINGLE);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick-DeltaTick);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n*MaxSize], Chunk);
						SendMsgEx(&Msg, MSGFLAG_FLUSH, i, true);
					}
					else
					{
						CMsgPacker Msg(NETMSG_SNAP);
						Msg.AddInt(m_CurrentGameTick);
						Msg.AddInt(m_CurrentGameTick-DeltaTick);
						Msg.AddInt(NumPackets);
						Msg.AddInt(n);
						Msg.AddInt(Crc);
						Msg.AddInt(Chunk);
						Msg.AddRaw(&aCompData[n*MaxSize], Chunk);
						SendMsgEx(&Msg, MSGFLAG_FLUSH, i, true);
					}
				}
			}
			else
			{
				CMsgPacker Msg(NETMSG_SNAPEMPTY);
				Msg.AddInt(m_CurrentGameTick);
				Msg.AddInt(m_CurrentGameTick-DeltaTick);
				SendMsgEx(&Msg, MSGFLAG_FLUSH, i, true);
			}
		}
	}

	p = m_pGames;
	while(p != NULL){
		p->GameServer()->OnPostSnap();
		p = p->m_pNext;
	}
}

int CServer::NewClientCallbackImpl(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	pThis->m_aClients[ClientID].m_aName[0] = 0;
	pThis->m_aClients[ClientID].m_aClan[0] = 0;
	pThis->m_aClients[ClientID].m_Country = -1;
	pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
	pThis->m_aClients[ClientID].m_AuthTries = 0;
	pThis->m_aClients[ClientID].m_uiGameID = GAME_ID_INVALID;
	pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;
	pThis->m_aClients[ClientID].m_Traffic = 0;
	pThis->m_aClients[ClientID].m_TrafficSince = 0;
	pThis->m_aClients[ClientID].m_PreferedTeam = -2;
	pThis->m_aClients[ClientID].Reset();

	++pThis->m_PlayerCount;

	return 0;
}

int CServer::NewClientNoAuthCallback(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;

	int r = NewClientCallbackImpl(ClientID, pUser);
	pThis->m_aClients[ClientID].m_State = CClient::STATE_CONNECTING;

	pThis->SendMap(ClientID);

	return r;
}

int CServer::NewClientCallback(int ClientID, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	int r = NewClientCallbackImpl(ClientID, pUser);
	pThis->m_aClients[ClientID].m_State = CClient::STATE_AUTH;
	return r;
}

int CServer::DelClientCallback(int ClientID, const char *pReason, void *pUser, bool ForceDisconnect)
{
	CServer *pThis = (CServer *)pUser;
	bool CanDrop = true;

	// notify the mod about the drop, if the mod says, that the connection can't be free'd, we don't drop the connection
	if(pThis->m_aClients[ClientID].m_State >= CClient::STATE_READY) {
		sGame* p = pThis->GetGame(pThis->m_aClients[ClientID].m_uiGameID);
		if(p != NULL) 
			CanDrop = p->GameServer()->OnClientDrop(ClientID, pReason, ForceDisconnect);
		
	}

	if (CanDrop) {
		char aAddrStr[NETADDR_MAXSTRSIZE];
		net_addr_str(pThis->m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "client dropped. cid=%d addr=%s reason='%s'", ClientID, aAddrStr, pReason);
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);

		pThis->m_aClients[ClientID].m_State = CClient::STATE_EMPTY;
		pThis->m_aClients[ClientID].m_aName[0] = 0;
		pThis->m_aClients[ClientID].m_aClan[0] = 0;
		pThis->m_aClients[ClientID].m_Country = -1;
		pThis->m_aClients[ClientID].m_Authed = AUTHED_NO;
		pThis->m_aClients[ClientID].m_AuthTries = 0;
		pThis->m_aClients[ClientID].m_uiGameID = GAME_ID_INVALID;
		pThis->m_aClients[ClientID].m_pRconCmdToSend = 0;
		pThis->m_aClients[ClientID].m_Traffic = 0;
		pThis->m_aClients[ClientID].m_TrafficSince = 0;
		pThis->m_aClients[ClientID].m_PreferedTeam = -2;
		pThis->m_aClients[ClientID].m_Snapshots.PurgeAll();
		--pThis->m_PlayerCount;
	}
	else
		return -1;

	// Check if connection is from a proxy
	if(g_Config.m_SvProxyCheck)
	{
		const NETADDR *pAddr = pThis->m_NetServer.ClientAddr(ClientID);

		if(pAddr)
		{
			if(pThis->IsProxy(pAddr))
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "Proxy connections are not allowed");
				pThis->m_NetServer.Drop(ClientID, aBuf);
				return 0;
			}
		}
	}

	return 0;
}

static int lastsent[MAX_CLIENTS];
static int lastask[MAX_CLIENTS];
static int lastasktick[MAX_CLIENTS];

void CServer::SendMap(int ClientID)
{
	lastsent[ClientID] = 0;
	lastask[ClientID] = 0;
	lastasktick[ClientID] = Tick();
	CMsgPacker Msg(NETMSG_MAP_CHANGE);
	Msg.AddString(GetMapName(), 0);
	Msg.AddInt(m_CurrentMapCrc);
	Msg.AddInt(m_CurrentMapSize);
	SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID, true);
}

void CServer::SendMap(int ClientID, unsigned int pGameID)
{
	if (pGameID == GAME_ID_INVALID || pGameID == 0) {
		SendMap(ClientID);
	}
	else {
		sMap* map = m_pMaps;
		while (map) {
			if (map->m_uiGameID == pGameID) break;
			map = map->m_pNextMap;
		}
		if (!map) return;
		
		lastsent[ClientID] = 0;
		lastask[ClientID] = 0;
		lastasktick[ClientID] = Tick();

		CMsgPacker Msg(NETMSG_MAP_CHANGE);
		Msg.AddString(map->m_aCurrentMap, 0);
		Msg.AddInt(map->m_CurrentMapCrc);
		Msg.AddInt(map->m_CurrentMapSize);
		SendMsgEx(&Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH, ClientID, true);
	}
}

void CServer::SendConnectionReady(int ClientID)
{
	CMsgPacker Msg(NETMSG_CON_READY);
	SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID, true);
}

void CServer::SendRconLine(int ClientID, const char *pLine)
{
	CMsgPacker Msg(NETMSG_RCON_LINE);
	Msg.AddString(pLine, 512);
	SendMsgEx(&Msg, MSGFLAG_VITAL, ClientID, true);
}

void CServer::SendRconLineAuthed(const char *pLine, void *pUser)
{
	CServer *pThis = (CServer *)pUser;
	static volatile int ReentryGuard = 0;
	int i;

	if(ReentryGuard) return;
	ReentryGuard++;

	for(i = 0; i < MAX_CLIENTS; i++)
	{
		if(pThis->m_aClients[i].m_State != CClient::STATE_EMPTY && pThis->m_aClients[i].m_Authed >= pThis->m_RconAuthLevel)
			pThis->SendRconLine(i, pLine);
	}

	ReentryGuard--;
}

void CServer::SendRconCmdAdd(const IConsole::CCommandInfo *pCommandInfo, int ClientID)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_ADD);
	Msg.AddString(pCommandInfo->m_pName, IConsole::TEMPCMD_NAME_LENGTH);
	Msg.AddString(pCommandInfo->m_pHelp, IConsole::TEMPCMD_HELP_LENGTH);
	Msg.AddString(pCommandInfo->m_pParams, IConsole::TEMPCMD_PARAMS_LENGTH);
	SendMsgEx(&Msg, MSGFLAG_VITAL, ClientID, true);
}

void CServer::SendRconCmdRem(const IConsole::CCommandInfo *pCommandInfo, int ClientID)
{
	CMsgPacker Msg(NETMSG_RCON_CMD_REM);
	Msg.AddString(pCommandInfo->m_pName, 256);
	SendMsgEx(&Msg, MSGFLAG_VITAL, ClientID, true);
}

void CServer::UpdateClientRconCommands()
{
	int ClientID = Tick() % MAX_CLIENTS;

	if(m_aClients[ClientID].m_State != CClient::STATE_EMPTY && m_aClients[ClientID].m_Authed)
	{
		int ConsoleAccessLevel = m_aClients[ClientID].m_Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : IConsole::ACCESS_LEVEL_MOD;
		for(int i = 0; i < MAX_RCONCMD_SEND && m_aClients[ClientID].m_pRconCmdToSend; ++i)
		{
			SendRconCmdAdd(m_aClients[ClientID].m_pRconCmdToSend, ClientID);
			m_aClients[ClientID].m_pRconCmdToSend = m_aClients[ClientID].m_pRconCmdToSend->NextCommandInfo(ConsoleAccessLevel, CFGFLAG_SERVER);
		}
	}
}

void CServer::ProcessClientPacket(CNetChunk *pPacket)
{
	int ClientID = pPacket->m_ClientID;
	CUnpacker Unpacker;
	Unpacker.Reset(pPacket->m_pData, pPacket->m_DataSize);

	// unpack msgid and system flag
	int Msg = Unpacker.GetInt();
	int Sys = Msg&1;
	Msg >>= 1;

	if(Unpacker.Error())
		return;
	
	if(g_Config.m_SvNetlimit && Msg != NETMSG_REQUEST_MAP_DATA)
	{
		int64 Now = time_get();
		int64 Diff = Now - m_aClients[ClientID].m_TrafficSince;
		float Alpha = g_Config.m_SvNetlimitAlpha / 100.0;
		float Limit = (float) g_Config.m_SvNetlimit * 1024 / time_freq();

		if (m_aClients[ClientID].m_Traffic > Limit)
		{
			m_NetServer.NetBan()->BanAddr(&pPacket->m_Address, 600, "Stressing network");
			return;
		}
		if (Diff > 100)
		{
			m_aClients[ClientID].m_Traffic = (Alpha * ((float) pPacket->m_DataSize / Diff)) + (1.0 - Alpha) * m_aClients[ClientID].m_Traffic;
			m_aClients[ClientID].m_TrafficSince = Now;
		}
	}

	if(Sys)
	{
		// system message
		if(Msg == NETMSG_INFO)
		{
			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State == CClient::STATE_AUTH)
			{
				const char *pVersion = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				if(str_comp(pVersion, GameServer()->NetVersion()) != 0)
				{
					// wrong version
					char aReason[256];
					str_format(aReason, sizeof(aReason), "Wrong version. Server is running '%s' and client '%s'", GameServer()->NetVersion(), pVersion);
					m_NetServer.Drop(ClientID, aReason);
					return;
				}

				const char *pPassword = Unpacker.GetString(CUnpacker::SANITIZE_CC);
				if(g_Config.m_Password[0] != 0 && str_comp(g_Config.m_Password, pPassword) != 0)
				{
					// wrong password
					m_NetServer.Drop(ClientID, "Wrong password");
					return;
				}

				m_aClients[ClientID].m_State = CClient::STATE_CONNECTING;
				SendMap(ClientID);
			}
		}
		else if(Msg == NETMSG_REQUEST_MAP_DATA)
		{
			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) == 0 || m_aClients[ClientID].m_State < CClient::STATE_CONNECTING)
				return;
	
			if(m_aClients[ClientID].m_uiGameID == GAME_ID_INVALID || m_aClients[ClientID].m_uiGameID == 0){
				int Chunk = Unpacker.GetInt();
				int ChunkSize = 1024-128;
				int Offset = Chunk * ChunkSize;
				int Last = 0;

				//ddnet code
				lastask[ClientID] = Chunk;
				lastasktick[ClientID] = Tick();
				if (Chunk == 0)
				{
					lastsent[ClientID] = Chunk;
				}
			
				// drop faulty map data requests
				if(Chunk < 0 || Offset > m_CurrentMapSize)
					return;

				if(Offset+ChunkSize >= m_CurrentMapSize)
				{
					ChunkSize = m_CurrentMapSize-Offset;
					if(ChunkSize < 0)
						ChunkSize = 0;
					Last = 1;
				}
				
				if (lastsent[ClientID] < Chunk+g_Config.m_SvMapWindow && g_Config.m_SvHighBandwidth)
					return;

				CMsgPacker Msg(NETMSG_MAP_DATA);
				Msg.AddInt(Last);
				Msg.AddInt(m_CurrentMapCrc);
				Msg.AddInt(Chunk);
				Msg.AddInt(ChunkSize);
				Msg.AddRaw(&m_pCurrentMapData[Offset], ChunkSize);
				SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID, true);

				if(g_Config.m_Debug)
				{
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
					Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
				}				
			} else {
				sMap* mapToDownload = m_pMaps;
				while(mapToDownload){
					if(mapToDownload->m_uiGameID == m_aClients[ClientID].m_uiGameID){
						break;
					}
					mapToDownload = mapToDownload->m_pNextMap;
				}
				
				if(mapToDownload){
					int Chunk = Unpacker.GetInt();
					int ChunkSize = 1024-128;
					int Offset = Chunk * ChunkSize;
					int Last = 0;

					//ddnet code
					lastask[ClientID] = Chunk;
					lastasktick[ClientID] = Tick();
					if (Chunk == 0)
					{
						lastsent[ClientID] = Chunk;
					}

					// drop faulty map data requests
					if(Chunk < 0 || Offset > mapToDownload->m_CurrentMapSize)
						return;

					if(Offset+ChunkSize >= mapToDownload->m_CurrentMapSize)
					{
						ChunkSize = mapToDownload->m_CurrentMapSize-Offset;
						if(ChunkSize < 0)
							ChunkSize = 0;
						Last = 1;
					}
				
					if (lastsent[ClientID] < Chunk+g_Config.m_SvMapWindow && g_Config.m_SvHighBandwidth)
						return;

					CMsgPacker Msg(NETMSG_MAP_DATA);
					Msg.AddInt(Last);
					Msg.AddInt(mapToDownload->m_CurrentMapCrc);
					Msg.AddInt(Chunk);
					Msg.AddInt(ChunkSize);
					Msg.AddRaw(&mapToDownload->m_pCurrentMapData[Offset], ChunkSize);
					SendMsgEx(&Msg, MSGFLAG_VITAL|MSGFLAG_FLUSH, ClientID, true);

					if(g_Config.m_Debug)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
						Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
					}
				}
			}
		}
		else if(Msg == NETMSG_READY)
		{
			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State == CClient::STATE_CONNECTING)
			{
				char aAddrStr[NETADDR_MAXSTRSIZE];
				net_addr_str(m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "player is ready. ClientID=%x addr=%s", ClientID, aAddrStr);
				Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);
				m_aClients[ClientID].m_State = CClient::STATE_READY;
				if(m_aClients[ClientID].m_uiGameID == GAME_ID_INVALID) {
					GameServer()->OnClientConnected(ClientID, m_aClients[ClientID].m_PreferedTeam);
					m_aClients[ClientID].m_uiGameID = 0;
				}
				else {		
					sGame* p = GetGame(m_aClients[ClientID].m_uiGameID);
					if(p != NULL) p->GameServer()->OnClientConnected(ClientID, m_aClients[ClientID].m_PreferedTeam);					
				}
				SendConnectionReady(ClientID);
			}
		}
		else if(Msg == NETMSG_ENTERGAME)
		{
			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State == CClient::STATE_READY){
				bool isReady = false;
				
				sGame* p = GetGame(m_aClients[ClientID].m_uiGameID);
				if(p != NULL) isReady = p->GameServer()->IsClientReady(ClientID);
				
				if(isReady) {
					char aAddrStr[NETADDR_MAXSTRSIZE];
					net_addr_str(m_NetServer.ClientAddr(ClientID), aAddrStr, sizeof(aAddrStr), true);

					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "player has entered the game. ClientID=%x addr=%s", ClientID, aAddrStr);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
					m_aClients[ClientID].m_State = CClient::STATE_INGAME;					
				
					sGame* p = GetGame(m_aClients[ClientID].m_uiGameID);
					if(p != NULL) p->GameServer()->OnClientEnter(ClientID);
				}
			}
		}
		else if(Msg == NETMSG_INPUT)
		{
			CClient::CInput *pInput;
			int64 TagTime;

			m_aClients[ClientID].m_LastAckedSnapshot = Unpacker.GetInt();
			int IntendedTick = Unpacker.GetInt();
			int Size = Unpacker.GetInt();

			// check for errors
			if(Unpacker.Error() || Size/4 > MAX_INPUT_SIZE)
				return;

			if(m_aClients[ClientID].m_LastAckedSnapshot > 0)
				m_aClients[ClientID].m_SnapRate = CClient::SNAPRATE_FULL;

			if(m_aClients[ClientID].m_Snapshots.Get(m_aClients[ClientID].m_LastAckedSnapshot, &TagTime, 0, 0) >= 0)
				m_aClients[ClientID].m_Latency = (int)(((time_get()-TagTime)*1000)/time_freq());

			// add message to report the input timing
			// skip packets that are old
			if(IntendedTick > m_aClients[ClientID].m_LastInputTick)
			{
				int TimeLeft = ((TickStartTime(IntendedTick)-time_get())*1000) / time_freq();

				CMsgPacker Msg(NETMSG_INPUTTIMING);
				Msg.AddInt(IntendedTick);
				Msg.AddInt(TimeLeft);
				SendMsgEx(&Msg, 0, ClientID, true);
			}

			m_aClients[ClientID].m_LastInputTick = IntendedTick;

			pInput = &m_aClients[ClientID].m_aInputs[m_aClients[ClientID].m_CurrentInput];

			if(IntendedTick <= Tick())
				IntendedTick = Tick()+1;

			pInput->m_GameTick = IntendedTick;

			for(int i = 0; i < Size/4; i++)
				pInput->m_aData[i] = Unpacker.GetInt();

			mem_copy(m_aClients[ClientID].m_LatestInput.m_aData, pInput->m_aData, MAX_INPUT_SIZE*sizeof(int));

			m_aClients[ClientID].m_CurrentInput++;
			m_aClients[ClientID].m_CurrentInput %= 200;

			// call the mod with the fresh input data
			if(m_aClients[ClientID].m_State == CClient::STATE_INGAME) {		
				sGame* p = GetGame(m_aClients[ClientID].m_uiGameID);
				if(p != NULL) p->GameServer()->OnClientDirectInput(ClientID, m_aClients[ClientID].m_LatestInput.m_aData);
			}
		}
		else if(Msg == NETMSG_RCON_CMD)
		{
			const char *pCmd = Unpacker.GetString();

			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && Unpacker.Error() == 0 && m_aClients[ClientID].m_Authed)
			{
				char aBuf[256];
				str_format(aBuf, sizeof(aBuf), "ClientID=%d rcon='%s'", ClientID, pCmd);
				Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBuf);
				m_RconClientID = ClientID;
				m_RconAuthLevel = m_aClients[ClientID].m_Authed;
				Console()->SetAccessLevel(m_aClients[ClientID].m_Authed == AUTHED_ADMIN ? IConsole::ACCESS_LEVEL_ADMIN : IConsole::ACCESS_LEVEL_MOD);
				Console()->ExecuteLineFlag(pCmd, CFGFLAG_SERVER);
				Console()->SetAccessLevel(IConsole::ACCESS_LEVEL_ADMIN);
				m_RconClientID = IServer::RCON_CID_SERV;
				m_RconAuthLevel = AUTHED_ADMIN;
			}
		}
		else if(Msg == NETMSG_RCON_AUTH)
		{
			const char *pPw;
			Unpacker.GetString(); // login name, not used
			pPw = Unpacker.GetString(CUnpacker::SANITIZE_CC);

			if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && Unpacker.Error() == 0)
			{
				if(g_Config.m_SvRconPassword[0] == 0 && g_Config.m_SvRconModPassword[0] == 0)
				{
					SendRconLine(ClientID, "No rcon password set on server. Set sv_rcon_password and/or sv_rcon_mod_password to enable the remote console.");
				}
				else if(g_Config.m_SvRconPassword[0] && str_comp(pPw, g_Config.m_SvRconPassword) == 0)
				{
					CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS);
					Msg.AddInt(1);	//authed
					Msg.AddInt(1);	//cmdlist
					SendMsgEx(&Msg, MSGFLAG_VITAL, ClientID, true);

					m_aClients[ClientID].m_Authed = AUTHED_ADMIN;
					int SendRconCmds = Unpacker.GetInt();
					if(Unpacker.Error() == 0 && SendRconCmds)
						m_aClients[ClientID].m_pRconCmdToSend = Console()->FirstCommandInfo(IConsole::ACCESS_LEVEL_ADMIN, CFGFLAG_SERVER);
					SendRconLine(ClientID, "Admin authentication successful. Full remote console access granted.");
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "ClientID=%d authed (admin)", ClientID);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				}
				else if(g_Config.m_SvRconModPassword[0] && str_comp(pPw, g_Config.m_SvRconModPassword) == 0)
				{
					CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS);
					Msg.AddInt(1);	//authed
					Msg.AddInt(1);	//cmdlist
					SendMsgEx(&Msg, MSGFLAG_VITAL, ClientID, true);

					m_aClients[ClientID].m_Authed = AUTHED_MOD;
					int SendRconCmds = Unpacker.GetInt();
					if(Unpacker.Error() == 0 && SendRconCmds)
						m_aClients[ClientID].m_pRconCmdToSend = Console()->FirstCommandInfo(IConsole::ACCESS_LEVEL_MOD, CFGFLAG_SERVER);
					SendRconLine(ClientID, "Moderator authentication successful. Limited remote console access granted.");
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "ClientID=%d authed (moderator)", ClientID);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
				}
				else if(g_Config.m_SvRconMaxTries)
				{
					m_aClients[ClientID].m_AuthTries++;
					char aBuf[128];
					str_format(aBuf, sizeof(aBuf), "Wrong password %d/%d.", m_aClients[ClientID].m_AuthTries, g_Config.m_SvRconMaxTries);
					SendRconLine(ClientID, aBuf);
					if(m_aClients[ClientID].m_AuthTries >= g_Config.m_SvRconMaxTries)
					{
						if(!g_Config.m_SvRconBantime)
							m_NetServer.Drop(ClientID, "Too many remote console authentication tries");
						else
							m_ServerBan.BanAddr(m_NetServer.ClientAddr(ClientID), g_Config.m_SvRconBantime*60, "Too many remote console authentication tries");
					}
				}
				else
				{
					SendRconLine(ClientID, "Wrong password.");
				}
			}
		}
		else if(Msg == NETMSG_PING)
		{
			CMsgPacker Msg(NETMSG_PING_REPLY);
			SendMsgEx(&Msg, 0, ClientID, true);
		}
		else
		{
			if(g_Config.m_Debug)
			{
				char aHex[] = "0123456789ABCDEF";
				char aBuf[512];

				for(int b = 0; b < pPacket->m_DataSize && b < 32; b++)
				{
					aBuf[b*3] = aHex[((const unsigned char *)pPacket->m_pData)[b]>>4];
					aBuf[b*3+1] = aHex[((const unsigned char *)pPacket->m_pData)[b]&0xf];
					aBuf[b*3+2] = ' ';
					aBuf[b*3+3] = 0;
				}

				char aBufMsg[256];
				str_format(aBufMsg, sizeof(aBufMsg), "strange message ClientID=%d msg=%d data_size=%d", ClientID, Msg, pPacket->m_DataSize);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBufMsg);
				Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
			}
		}
	}
	else
	{
		// game message
		if((pPacket->m_Flags&NET_CHUNKFLAG_VITAL) != 0 && m_aClients[ClientID].m_State >= CClient::STATE_READY){
			sGame* p = GetGame(m_aClients[ClientID].m_uiGameID);
			if(p != NULL) p->GameServer()->OnMessage(Msg, &Unpacker, ClientID);
		}
	}
}

void CServer::SendServerInfo(const NETADDR *pAddr, int Token, bool Extended)
{
	CNetChunk Packet;
	CPacker p;
	char aBuf[128];

	// count the players
	int PlayerCount = 0, ClientCount = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			bool InGame = false;
			sGame* p = GetGame(m_aClients[i].m_uiGameID);
			if(p != NULL) InGame = p->GameServer()->IsClientPlayer(i);
			if(InGame)
				PlayerCount++;

			ClientCount++;
		}
	}

	p.Reset();

	int MaxClients = m_NetServer.MaxClients();

	if(Extended) p.AddRaw(SERVERBROWSE_INFO64, sizeof(SERVERBROWSE_INFO64));
	else p.AddRaw(SERVERBROWSE_INFO, sizeof(SERVERBROWSE_INFO));

	str_format(aBuf, sizeof(aBuf), "%d", Token);
	p.AddString(aBuf, 6);

	p.AddString(GameServer()->Version(), 32); 
	//ddnet code
	if (Extended)
	{
		if (m_NetServer.MaxClients() <= DDNET_MAX_CLIENTS){
			p.AddString(g_Config.m_SvName, 256);
		} else
		{
			str_format(aBuf, sizeof(aBuf), "%s [%d/%d]", g_Config.m_SvName, ClientCount, m_NetServer.MaxClients());
			p.AddString(aBuf, 256);
		}
	}
	else
	{
		if (m_NetServer.MaxClients() <= VANILLA_MAX_CLIENTS)
			p.AddString(g_Config.m_SvName, 64);
		else
		{
			// 									v support 32 slots or more than 64
			str_format(aBuf, sizeof(aBuf), "%s 64+[%d/%d]", g_Config.m_SvName, ClientCount, m_NetServer.MaxClients());
			p.AddString(aBuf, 64);
		}
	}
	p.AddString(GetMapName(), 32);

	// gametype
	p.AddString(GameServer()->GameType(), 16);

	// flags
	int i = 0;
	if(g_Config.m_Password[0]) // password set
		i |= SERVER_FLAG_PASSWORD;
	str_format(aBuf, sizeof(aBuf), "%d", i);
	p.AddString(aBuf, 2);

	//Ddnet.tw code
	if (!Extended)
	{
		if (ClientCount >= VANILLA_MAX_CLIENTS)
		{
			if (ClientCount < MaxClients)
				ClientCount = VANILLA_MAX_CLIENTS - 1;
			else
				ClientCount = VANILLA_MAX_CLIENTS;
		}
		if (MaxClients > VANILLA_MAX_CLIENTS) MaxClients = VANILLA_MAX_CLIENTS;
	} else
	{
		if (ClientCount >= DDNET_MAX_CLIENTS)
		{
			if (ClientCount < MaxClients)
				ClientCount = DDNET_MAX_CLIENTS - 1;
			else
				ClientCount = DDNET_MAX_CLIENTS;
		}
		if (MaxClients > DDNET_MAX_CLIENTS) MaxClients = DDNET_MAX_CLIENTS;
	}

	if (PlayerCount > ClientCount)
		PlayerCount = ClientCount;

	str_format(aBuf, sizeof(aBuf), "%d", PlayerCount); p.AddString(aBuf, 3); // num players
	str_format(aBuf, sizeof(aBuf), "%d", ((m_NetServer.MaxClients() - g_Config.m_SvSpectatorSlots) > MaxClients) ? MaxClients : (m_NetServer.MaxClients() - g_Config.m_SvSpectatorSlots)); p.AddString(aBuf, 3); // max players
	str_format(aBuf, sizeof(aBuf), "%d", ClientCount); p.AddString(aBuf, 3); // num clients
	str_format(aBuf, sizeof(aBuf), "%d", MaxClients); p.AddString(aBuf, 3); // max clients

	//ddnet code
	if (Extended)
		p.AddInt(0);

	int count = 0;
	for(i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			if (!Extended && count >= VANILLA_MAX_CLIENTS) break;
			if (Extended && count >= DDNET_MAX_CLIENTS) break;
			++count;

			p.AddString(ClientName(i), MAX_NAME_LENGTH); // client name
			p.AddString(ClientClan(i), MAX_CLAN_LENGTH); // client clan
			str_format(aBuf, sizeof(aBuf), "%d", m_aClients[i].m_Country); p.AddString(aBuf, 6); // client country
			str_format(aBuf, sizeof(aBuf), "%d", m_aClients[i].m_Score); p.AddString(aBuf, 6); // client score
			bool InGame = false;
			sGame* g = GetGame(m_aClients[i].m_uiGameID);
			if(g != NULL) InGame = g->GameServer()->IsClientPlayer(i);
			str_format(aBuf, sizeof(aBuf), "%d", InGame?1:0); p.AddString(aBuf, 2); // is player?
		}
	}

	Packet.m_ClientID = -1;
	Packet.m_Address = *pAddr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS;
	Packet.m_DataSize = p.Size();
	Packet.m_pData = p.Data();
	m_NetServer.Send(&Packet);
}

void CServer::UpdateServerInfo()
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if (m_aClients[i].m_State != CClient::STATE_EMPTY) {
			SendServerInfo(m_NetServer.ClientAddr(i), -1, true);
			SendServerInfo(m_NetServer.ClientAddr(i), -1, false);
		}
	}
}


void CServer::PumpNetwork()
{
	CNetChunk Packet;

	m_NetServer.Update();

	// process packets
	while(m_NetServer.Recv(&Packet))
	{
		if(Packet.m_ClientID == -1)
		{
			// stateless
			if(!m_Register.RegisterProcessPacket(&Packet))
			{
				//ddnet code
				bool ServerInfo = false;
				bool Extended = false;

				if(Packet.m_DataSize == sizeof(SERVERBROWSE_GETINFO)+1 &&
					mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO)) == 0)
				{
					ServerInfo = true;
					Extended = false;
				} 
				else if (Packet.m_DataSize == sizeof(SERVERBROWSE_GETINFO64) + 1 &&
					mem_comp(Packet.m_pData, SERVERBROWSE_GETINFO64, sizeof(SERVERBROWSE_GETINFO64)) == 0)
				{
					ServerInfo = true;
					Extended = true;
				}
				if (ServerInfo) {

					SendServerInfo(&Packet.m_Address, ((unsigned char *)Packet.m_pData)[sizeof(SERVERBROWSE_GETINFO)], Extended);
				}
			}
		}
		else
			ProcessClientPacket(&Packet);
	}
	//ddnet code
	if(g_Config.m_SvHighBandwidth){
		for (int i=0;i<MAX_CLIENTS;i++)
		{
			if (m_aClients[i].m_State != CClient::STATE_CONNECTING)
				continue;
			if (lastasktick[i] < Tick()-TickSpeed())
			{
				lastsent[i] = lastask[i];
				lastasktick[i] = Tick();
			}
			if (lastask[i]<lastsent[i]-g_Config.m_SvMapWindow)
				continue;
			if(m_aClients[i].m_uiGameID == GAME_ID_INVALID || m_aClients[i].m_uiGameID == 0){
				int Chunk = lastsent[i]++;
				int ChunkSize = 1024-128;
				int Offset = Chunk * ChunkSize;
				int Last = 0;

				//ddnet code			
				// drop faulty map data requests
				if(Chunk < 0 || Offset > m_CurrentMapSize)
					return;

				if(Offset+ChunkSize >= m_CurrentMapSize)
				{
					ChunkSize = m_CurrentMapSize-Offset;
					if(ChunkSize < 0)
						ChunkSize = 0;
					Last = 1;
				}

				CMsgPacker Msg(NETMSG_MAP_DATA);
				Msg.AddInt(Last);
				Msg.AddInt(m_CurrentMapCrc);
				Msg.AddInt(Chunk);
				Msg.AddInt(ChunkSize);
				Msg.AddRaw(&m_pCurrentMapData[Offset], ChunkSize);
				SendMsgEx(&Msg, MSGFLAG_FLUSH, i, true);

				if(g_Config.m_Debug)
				{
					char aBuf[256];
					str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
					Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
				}				
			} else {
				sMap* mapToDownload = m_pMaps;
				while(mapToDownload){
					if(mapToDownload->m_uiGameID == m_aClients[i].m_uiGameID){
						break;
					}
					mapToDownload = mapToDownload->m_pNextMap;
				}
				
				if(mapToDownload){
					int Chunk = lastsent[i]++;
					int ChunkSize = 1024-128;
					int Offset = Chunk * ChunkSize;
					int Last = 0;

					//ddnet code
					// drop faulty map data requests
					if(Chunk < 0 || Offset > mapToDownload->m_CurrentMapSize)
						return;

					if(Offset+ChunkSize >= mapToDownload->m_CurrentMapSize)
					{
						ChunkSize = mapToDownload->m_CurrentMapSize-Offset;
						if(ChunkSize < 0)
							ChunkSize = 0;
						Last = 1;
					}

					CMsgPacker Msg(NETMSG_MAP_DATA);
					Msg.AddInt(Last);
					Msg.AddInt(mapToDownload->m_CurrentMapCrc);
					Msg.AddInt(Chunk);
					Msg.AddInt(ChunkSize);
					Msg.AddRaw(&mapToDownload->m_pCurrentMapData[Offset], ChunkSize);
					SendMsgEx(&Msg, MSGFLAG_FLUSH, i, true);

					if(g_Config.m_Debug)
					{
						char aBuf[256];
						str_format(aBuf, sizeof(aBuf), "sending chunk %d with size %d", Chunk, ChunkSize);
						Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
					}
				}
			}
		}
	}

	m_ServerBan.Update();
	m_Econ.Update();
}

char *CServer::GetMapName()
{
	// get the name of the map without his path
	char *pMapShortName = &g_Config.m_SvMap[0];
	for(int i = 0; i < str_length(g_Config.m_SvMap)-1; i++)
	{
		if(g_Config.m_SvMap[i] == '/' || g_Config.m_SvMap[i] == '\\')
			pMapShortName = &g_Config.m_SvMap[i+1];
	}
	return pMapShortName;
}

int CServer::LoadMap(const char *pMapName)
{
	//DATAFILE *df;
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);

	/*df = datafile_load(buf);
	if(!df)
		return 0;*/

	// check for valid standard map
	if(!m_MapChecker.ReadAndValidateMap(Storage(), aBuf, IStorage::TYPE_ALL))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mapchecker", "invalid standard map");
		return 0;
	}

	if(!m_pMap->Load(aBuf))
		return 0;

	// stop recording when we change map
	m_DemoRecorder.Stop();

	// reinit snapshot ids
	m_IDPool.TimeoutIDs();

	// get the crc of the map
	m_CurrentMapCrc = m_pMap->Crc();
	char aBufMsg[256];
	str_format(aBufMsg, sizeof(aBufMsg), "%s crc is %08x", aBuf, m_CurrentMapCrc);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBufMsg);

	str_copy(m_aCurrentMap, pMapName, sizeof(m_aCurrentMap));
	//map_set(df);

	// load complete map into memory for download
	{
		IOHANDLE File = Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
		m_CurrentMapSize = (int)io_length(File);
		if(m_pCurrentMapData)
			mem_free(m_pCurrentMapData);
		m_pCurrentMapData = (unsigned char *)mem_alloc(m_CurrentMapSize, 1);
		io_read(File, m_pCurrentMapData, m_CurrentMapSize);
		io_close(File);
	}
	return 1;
}

IMap* CServer::LoadAndGetMap(const char *pMapName, unsigned int pGameID)
{
	IEngineMap *pEngineMap = CreateEngineMap();

	//DATAFILE *df;
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);

	/*df = datafile_load(buf);
	if(!df)
	return 0;*/

	// check for valid standard map
	if (!m_MapChecker.ReadAndValidateMap(Storage(), aBuf, IStorage::TYPE_ALL))
	{
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mapchecker", "invalid standard map");
		return 0;
	}

	if (!pEngineMap->Load(aBuf, Kernel()))
	{
		delete pEngineMap;
		return 0;
	}

	sMap* map = m_pMaps;
	if (map == NULL) {
		map = new sMap;
		m_pMaps = map;
	}
	else {
		while (map->m_pNextMap != NULL) {
			map = map->m_pNextMap;
		}
		if (map->m_pNextMap == NULL) {
			map->m_pNextMap = new sMap;
			map = map->m_pNextMap;
		}
	}
	map->m_pMap = pEngineMap;
	map->m_uiGameID = pGameID;
	
	// get the crc of the map
	map->m_CurrentMapCrc = pEngineMap->Crc();
	char aBufMsg[256];
	str_format(aBufMsg, sizeof(aBufMsg), "%s crc is %08x", aBuf, map->m_CurrentMapCrc);
	Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBufMsg);

	str_copy(map->m_aCurrentMap, pMapName, sizeof(map->m_aCurrentMap));
	//map_set(df);

	// load complete map into memory for download
	{
		IOHANDLE File = Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
		map->m_CurrentMapSize = (int)io_length(File);
		if (map->m_pCurrentMapData)
			mem_free(map->m_pCurrentMapData);
		map->m_pCurrentMapData = (unsigned char *)mem_alloc(map->m_CurrentMapSize, 1);
		io_read(File, map->m_pCurrentMapData, map->m_CurrentMapSize);
		io_close(File);
	}

	return pEngineMap;
}


bool CServer::ChangeMap(const char *pMapName, unsigned int pGameID){
	sMap* pMap = m_pMaps;
	while(pMap){
		if(pMap->m_uiGameID == pGameID){
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "maps/%s.map", pMapName);
			
			// check for valid standard map
			if (!m_MapChecker.ReadAndValidateMap(Storage(), aBuf, IStorage::TYPE_ALL))
			{
				Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "mapchecker", "invalid standard map");
				return false;
			}

			IEngineMap* pEngineMap = ((IEngineMap*)pMap->m_pMap);
			if (!pEngineMap->Load(aBuf, Kernel()))
			{
				delete pEngineMap;
				return false;
			}

			// get the crc of the map
			pMap->m_CurrentMapCrc = pEngineMap->Crc();
			char aBufMsg[256];
			str_format(aBufMsg, sizeof(aBufMsg), "%s crc is %08x", aBuf, pMap->m_CurrentMapCrc);
			Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "server", aBufMsg);

			str_copy(pMap->m_aCurrentMap, pMapName, sizeof(pMap->m_aCurrentMap));
			//map_set(df);

			// load complete map into memory for download
			{
				IOHANDLE File = Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
				pMap->m_CurrentMapSize = (int)io_length(File);
				if (pMap->m_pCurrentMapData)
					mem_free(pMap->m_pCurrentMapData);
				pMap->m_pCurrentMapData = (unsigned char *)mem_alloc(pMap->m_CurrentMapSize, 1);
				io_read(File, pMap->m_pCurrentMapData, pMap->m_CurrentMapSize);
				io_close(File);
			}
			return true;
			break;
		}
		pMap = pMap->m_pNextMap;
	}
	return false;
}
	
void CServer::InitRegister(CNetServer *pNetServer, IEngineMasterServer *pMasterServer, IConsole *pConsole)
{
	m_Register.Init(pNetServer, pMasterServer, pConsole);
}

int CServer::Run()
{
	//
	m_PrintCBIndex = Console()->RegisterPrintCallback(g_Config.m_ConsoleOutputLevel, SendRconLineAuthed, this);

	// load map
	if(!LoadMap(g_Config.m_SvMap))
	{
		dbg_msg("server", "failed to load map. mapname='%s'", g_Config.m_SvMap);
		return -1;
	}

	if(g_Config.m_SvSqliteFile[0] != '\0')
	{
		char aFullPath[IO_MAX_PATH_LENGTH];
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, g_Config.m_SvSqliteFile, aFullPath, sizeof(aFullPath));

		if(g_Config.m_SvUseSql)
		{
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::Mode::WRITE_BACKUP, aFullPath);
		}
		else
		{
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::Mode::READ, aFullPath);
			DbPool()->RegisterSqliteDatabase(CDbConnectionPool::Mode::WRITE, aFullPath);
		}
	}

	// start server
	NETADDR BindAddr;
	if(g_Config.m_Bindaddr[0] && net_host_lookup(g_Config.m_Bindaddr, &BindAddr, NETTYPE_ALL) == 0)
	{
		// sweet!
		BindAddr.type = NETTYPE_ALL;
		BindAddr.port = g_Config.m_SvPort;
	}
	else
	{
		mem_zero(&BindAddr, sizeof(BindAddr));
		BindAddr.type = NETTYPE_ALL;
		BindAddr.port = g_Config.m_SvPort;
	}

	if(!m_NetServer.Open(BindAddr, &m_ServerBan, g_Config.m_SvMaxClients, g_Config.m_SvMaxClientsPerIP, 0))
	{
		dbg_msg("server", "couldn't open socket. port %d might already be in use", g_Config.m_SvPort);
		return -1;
	}

	m_NetServer.SetCallbacks(NewClientCallback, NewClientNoAuthCallback, DelClientCallback, this);

	m_Econ.Init(Console(), &m_ServerBan);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "server name is '%s'", g_Config.m_SvName);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	GameServer()->OnInit();
	str_format(aBuf, sizeof(aBuf), "version %s", GameServer()->NetVersion());
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);

	// process pending commands
	m_pConsole->StoreCommands(false);

	// start game
	{
		int64 ReportTime = time_get();
		int ReportInterval = 3;

		m_Lastheartbeat = 0;
		m_GameStartTime = time_get();

		if(g_Config.m_Debug)
		{
			str_format(aBuf, sizeof(aBuf), "baseline memory usage %dk", mem_stats()->allocated/1024);
			Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "server", aBuf);
		}

		while(m_RunServer)
		{
			int64 t = time_get();
			int NewTicks = 0;

			// load new map TODO: don't poll this
			if(str_comp(g_Config.m_SvMap, m_aCurrentMap) != 0 || m_MapReload)
			{
				m_MapReload = 0;

				// load map
				if(LoadMap(g_Config.m_SvMap))
				{
					int aPreferedTeams[MAX_CLIENTS];

					for(int c = 0; c < MAX_CLIENTS; c++)
					{
						aPreferedTeams[c] = GameServer()->PreferedTeamPlayer(c);
					}

					// new map loaded
					GameServer()->OnShutdown();

					for(int c = 0; c < MAX_CLIENTS; c++)
					{
						if(m_aClients[c].m_State <= CClient::STATE_AUTH)
							continue;

						SendMap(c);
						m_aClients[c].Reset();
						m_aClients[c].m_State = CClient::STATE_CONNECTING;
						m_aClients[c].m_PreferedTeam = aPreferedTeams[c];
					}

					m_GameStartTime = time_get();
					m_CurrentGameTick = 0;
					Kernel()->ReregisterInterface(GameServer());
					GameServer()->OnInit();
					UpdateServerInfo();
				}
				else
				{
					str_format(aBuf, sizeof(aBuf), "failed to load map. mapname='%s'", g_Config.m_SvMap);
					Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
					str_copy(g_Config.m_SvMap, m_aCurrentMap, sizeof(g_Config.m_SvMap));
				}
			}

			while(t > TickStartTime(m_CurrentGameTick+1))
			{
				m_CurrentGameTick++;
				NewTicks++;

				if(m_PlayerCount){
					// apply new input
					for(int c = 0; c < MAX_CLIENTS; c++)
					{
						if(m_aClients[c].m_State == CClient::STATE_EMPTY)
							continue;
						for(int i = 0; i < 200; i++)
						{
							if(m_aClients[c].m_aInputs[i].m_GameTick == Tick())
							{
								if(m_aClients[c].m_State == CClient::STATE_INGAME) {
									sGame* p = GetGame(m_aClients[c].m_uiGameID);
									if(p != NULL) p->GameServer()->OnClientPredictedInput(c, m_aClients[c].m_aInputs[i].m_aData);				
								}
								break;
							}
						}
					}

					sGame* p = m_pGames;
					while(p != NULL){	
						p->GameServer()->OnTick();
						p = p->m_pNext;
					}
				} else {
					if(m_StopServerWhenEmpty) m_RunServer = 0;
				}
			}

			// snap game
			if(NewTicks)
			{
				if(g_Config.m_SvHighBandwidth || (m_CurrentGameTick%2) == 0)
					DoSnapshot();

				UpdateClientRconCommands();
			}

			// master server stuff
			m_Register.RegisterUpdate(m_NetServer.NetType());

			PumpNetwork();

			if(ReportTime < time_get())
			{
				if(g_Config.m_Debug)
				{
					/*
					static NETSTATS prev_stats;
					NETSTATS stats;
					netserver_stats(net, &stats);

					perf_next();

					if(config.dbg_pref)
						perf_dump(&rootscope);

					dbg_msg("server", "send=%8d recv=%8d",
						(stats.send_bytes - prev_stats.send_bytes)/reportinterval,
						(stats.recv_bytes - prev_stats.recv_bytes)/reportinterval);

					prev_stats = stats;
					*/
				}

				ReportTime += time_freq()*ReportInterval;
			}

			// wait for incomming data
			net_socket_read_wait(m_NetServer.Socket(), 5);
		}
	}
	// disconnect all clients on shutdown
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(m_aClients[i].m_State != CClient::STATE_EMPTY)
			m_NetServer.Drop(i, "Server shutdown");

		m_Econ.Shutdown();
	}

	GameServer()->OnShutdown();
	m_pMap->Unload();

	if(m_pCurrentMapData)
		mem_free(m_pCurrentMapData);
	return 0;
}

void CServer::ConKick(IConsole::IResult *pResult, void *pUser)
{
	if(pResult->NumArguments() > 1)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Kicked (%s)", pResult->GetString(1));
		((CServer *)pUser)->Kick(pResult->GetInteger(0), aBuf);
	}
	else
		((CServer *)pUser)->Kick(pResult->GetInteger(0), "Kicked by console");
}

void CServer::ConStatus(IConsole::IResult *pResult, void *pUser)
{
	char aBuf[1024];
	char aAddrStr[NETADDR_MAXSTRSIZE];
	CServer* pThis = static_cast<CServer *>(pUser);

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(pThis->m_aClients[i].m_State != CClient::STATE_EMPTY)
		{
			net_addr_str(pThis->m_NetServer.ClientAddr(i), aAddrStr, sizeof(aAddrStr), true);
			if(pThis->m_aClients[i].m_State == CClient::STATE_INGAME)
			{
				const char *pAuthStr = pThis->m_aClients[i].m_Authed == CServer::AUTHED_ADMIN ? "(Admin)" :
										pThis->m_aClients[i].m_Authed == CServer::AUTHED_MOD ? "(Mod)" : "";
										
				char pVersionStr[50];
				if(pThis->m_aClients[i].m_Version != -1) str_format(pVersionStr, sizeof(pVersionStr), "Client Version: %d", pThis->m_aClients[i].m_Version);
				else pVersionStr[0] = 0;
				char pFlagsStr[50];
				if(pThis->m_aClients[i].m_UnknownFlags != 0 && pThis->m_aClients[i].m_UnknownFlags != 0x10/*DDNet Hookcollision*/) str_format(pFlagsStr, sizeof(pFlagsStr), "Unknown Flags: %d", pThis->m_aClients[i].m_UnknownFlags);
				else pFlagsStr[0] = 0;
				str_format(aBuf, sizeof(aBuf), "id=%d addr=%s name='%s' score=%d %s %s %s", i, aAddrStr,
					pThis->m_aClients[i].m_aName, pThis->m_aClients[i].m_Score, pAuthStr, pVersionStr, pFlagsStr);
			}
			else
				str_format(aBuf, sizeof(aBuf), "id=%d addr=%s connecting", i, aAddrStr);
			pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", aBuf);
		}
	}
}

void CServer::ConShutdown(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_RunServer = 0;
}

void CServer::ConShutdownEmpty(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_StopServerWhenEmpty = 1;
}

void CServer::DemoRecorder_HandleAutoStart()
{
	if(g_Config.m_SvAutoDemoRecord)
	{
		m_DemoRecorder.Stop();
		char aFilename[128];
		char aDate[20];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "demos/%s_%s.demo", "auto/autorecord", aDate);
		m_DemoRecorder.Start(Storage(), m_pConsole, aFilename, GameServer()->NetVersion(), m_aCurrentMap, m_CurrentMapCrc, "server");
		if(g_Config.m_SvAutoDemoMax)
		{
			// clean up auto recorded demos
			CFileCollection AutoDemos;
			AutoDemos.Init(Storage(), "demos/server", "autorecord", ".demo", g_Config.m_SvAutoDemoMax);
		}
	}
}

bool CServer::DemoRecorder_IsRecording()
{
	return m_DemoRecorder.IsRecording();
}

void CServer::ConRecord(IConsole::IResult *pResult, void *pUser)
{
	CServer* pServer = (CServer *)pUser;
	char aFilename[128];

	if(pResult->NumArguments())
		str_format(aFilename, sizeof(aFilename), "demos/%s.demo", pResult->GetString(0));
	else
	{
		char aDate[20];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "demos/demo_%s.demo", aDate);
	}
	pServer->m_DemoRecorder.Start(pServer->Storage(), pServer->Console(), aFilename, pServer->GameServer()->NetVersion(), pServer->m_aCurrentMap, pServer->m_CurrentMapCrc, "server");
}

void CServer::ConStopRecord(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_DemoRecorder.Stop();
}

void CServer::ConMapReload(IConsole::IResult *pResult, void *pUser)
{
	((CServer *)pUser)->m_MapReload = 1;
}

void CServer::ConLogout(IConsole::IResult *pResult, void *pUser)
{
	CServer *pServer = (CServer *)pUser;

	if(pServer->m_RconClientID >= 0 && pServer->m_RconClientID < MAX_CLIENTS &&
		pServer->m_aClients[pServer->m_RconClientID].m_State != CServer::CClient::STATE_EMPTY)
	{
		CMsgPacker Msg(NETMSG_RCON_AUTH_STATUS);
		Msg.AddInt(0);	//authed
		Msg.AddInt(0);	//cmdlist
		pServer->SendMsgEx(&Msg, MSGFLAG_VITAL, pServer->m_RconClientID, true);

		pServer->m_aClients[pServer->m_RconClientID].m_Authed = AUTHED_NO;
		pServer->m_aClients[pServer->m_RconClientID].m_AuthTries = 0;
		pServer->m_aClients[pServer->m_RconClientID].m_pRconCmdToSend = 0;
		pServer->SendRconLine(pServer->m_RconClientID, "Logout successful.");
		char aBuf[32];
		str_format(aBuf, sizeof(aBuf), "ClientID=%d logged out", pServer->m_RconClientID);
		pServer->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
	}
}

void CServer::ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CServer *)pUserData)->UpdateServerInfo();
}

void CServer::ConchainMaxclientsperipUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CServer *)pUserData)->m_NetServer.SetMaxClientsPerIP(pResult->GetInteger(0));
}

void CServer::ConchainModCommandUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	if(pResult->NumArguments() == 2)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		const IConsole::CCommandInfo *pInfo = pThis->Console()->GetCommandInfo(pResult->GetString(0), CFGFLAG_SERVER, false);
		int OldAccessLevel = 0;
		if(pInfo)
			OldAccessLevel = pInfo->GetAccessLevel();
		pfnCallback(pResult, pCallbackUserData);
		if(pInfo && OldAccessLevel != pInfo->GetAccessLevel())
		{
			for(int i = 0; i < MAX_CLIENTS; ++i)
			{
				if(pThis->m_aClients[i].m_State == CServer::CClient::STATE_EMPTY || pThis->m_aClients[i].m_Authed != CServer::AUTHED_MOD ||
					(pThis->m_aClients[i].m_pRconCmdToSend && str_comp(pResult->GetString(0), pThis->m_aClients[i].m_pRconCmdToSend->m_pName) >= 0))
					continue;

				if(OldAccessLevel == IConsole::ACCESS_LEVEL_ADMIN)
					pThis->SendRconCmdAdd(pInfo, i);
				else
					pThis->SendRconCmdRem(pInfo, i);
			}
		}
	}
	else
		pfnCallback(pResult, pCallbackUserData);
}

void CServer::ConchainConsoleOutputLevelUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() == 1)
	{
		CServer *pThis = static_cast<CServer *>(pUserData);
		pThis->Console()->SetPrintOutputLevel(pThis->m_PrintCBIndex, pResult->GetInteger(0));
	}
}

void CServer::ConStartGame(IConsole::IResult *pResult, void *pUser){
	CServer *pThis = static_cast<CServer *>(pUser);
	
	if(pResult->NumArguments() == 1)
	{	
		pThis->StartGameServer(pResult->GetString(0));
	}
}

void CServer::ConStopGame(IConsole::IResult *pResult, void *pUser){
	CServer *pThis = static_cast<CServer *>(pUser);
	
	if(pResult->NumArguments() == 1)
	{
		unsigned int GameID = pResult->GetInteger(0);
		if(GameID == 0) return; //cant stop the "main" game server
		
		pThis->StopGameServer(GameID);
	}
}

void CServer::ConMovePlayerToGame(IConsole::IResult *pResult, void *pUser){
	CServer *pThis = static_cast<CServer *>(pUser);
	
	if(pResult->NumArguments() == 2)
	{
		int ID = pResult->GetInteger(0);
		int GameID = pResult->GetInteger(1);
		pThis->MovePlayerToGameServer(ID, GameID);
	}
}

void CServer::ConServerStatus(IConsole::IResult *pResult, void *pUser){
	CServer *pThis = static_cast<CServer *>(pUser);
	
	char aBuf[1024];

	sGame* pGame = pThis->m_pGames;
	while(pGame){
		sMap* pMap = pThis->m_pMaps;
		while(pMap){
			if(pMap->m_uiGameID == pGame->m_uiGameID) break;
			pMap = pMap->m_pNextMap;
		}
		
		str_format(aBuf, sizeof(aBuf), "id=%u map=%s", pGame->m_uiGameID, (pMap) ? pMap->m_aCurrentMap : ((pGame->m_uiGameID == 0) ? pThis->m_aCurrentMap : ""));
		pThis->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "Server", aBuf);
		pGame = pGame->m_pNext;
	}
}

void CServer::RegisterCommands()
{
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pGames->m_pGameServer = Kernel()->RequestInterface<IGameServer>();
	m_pGames->m_uiGameID = 0;
	m_pMap = Kernel()->RequestInterface<IEngineMap>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();

	// register console commands
	Console()->Register("kick", "i?r", CFGFLAG_SERVER, ConKick, this, "Kick player with specified id for any reason");
	Console()->Register("startgame", "s", CFGFLAG_SERVER, ConStartGame, this, "Start a game with a specific map");
	Console()->Register("stopgame", "i", CFGFLAG_SERVER, ConStopGame, this, "Stop a game by it's ID");
	Console()->Register("moveplayergame", "i?i", CFGFLAG_SERVER, ConMovePlayerToGame, this, "Move a player by id to a game by id");
	Console()->Register("serverstatus", "", CFGFLAG_SERVER, ConServerStatus, this, "List all game server");
	Console()->Register("status", "", CFGFLAG_SERVER, ConStatus, this, "List players");
	Console()->Register("shutdown", "", CFGFLAG_SERVER, ConShutdown, this, "Shut down");
	Console()->Register("shutdownwhenempty", "", CFGFLAG_SERVER, ConShutdownEmpty, this, "Shut down, when the server is empty");
	Console()->Register("logout", "", CFGFLAG_SERVER, ConLogout, this, "Logout of rcon");

	Console()->Register("record", "?s", CFGFLAG_SERVER|CFGFLAG_STORE, ConRecord, this, "Record to a file");
	Console()->Register("stoprecord", "", CFGFLAG_SERVER, ConStopRecord, this, "Stop recording");

	Console()->Register("reload", "", CFGFLAG_SERVER, ConMapReload, this, "Reload the map");

	Console()->Chain("sv_name", ConchainSpecialInfoupdate, this);
	Console()->Chain("password", ConchainSpecialInfoupdate, this);

	Console()->Chain("sv_max_clients_per_ip", ConchainMaxclientsperipUpdate, this);
	Console()->Chain("mod_command", ConchainModCommandUpdate, this);
	Console()->Chain("console_output_level", ConchainConsoleOutputLevelUpdate, this);

	// register console commands in sub parts
	m_ServerBan.InitServerBan(Console(), Storage(), this);
	m_pGames->m_pGameServer->OnConsoleInit();
}


int CServer::StartGameServer(const char* pMap, CConfiguration* pConfig){
	unsigned int freeGameID = 1;
		
	sGame* g = m_pGames;
	while(g->m_pNext){
		if(g->m_pNext->m_uiGameID != freeGameID) break;
		++freeGameID;
		g = g->m_pNext;
	}
	
	IMap* map = LoadAndGetMap(pMap, freeGameID);
	
	if(map){
		if(g->m_pNext){
			sGame* gtmp = g->m_pNext;
			
			g->m_pNext = new sGame;
			g = g->m_pNext;
			g->m_pNext = gtmp;
		} else {			
			g->m_pNext = new sGame;
			g = g->m_pNext;			
		}
		
		g->m_pGameServer = CreateGameServer();
		g->m_uiGameID = freeGameID;
		
		/*for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(pThis->m_aClients[c].m_State <= CClient::STATE_AUTH)
				continue;
			pThis->GameServer()->OnClientDrop(c, "", true);
			pThis->m_aClients[c].m_uiGameID = freeGameID;
			pThis->SendMap(c, freeGameID);
			pThis->m_aClients[c].Reset();
			pThis->m_aClients[c].m_State = CClient::STATE_CONNECTING;
		}*/

		if(pConfig) g->m_pGameServer->OnInit(Kernel(), map, pConfig);
		else g->m_pGameServer->OnInit(Kernel(), map);
	} else return -1;
	
	
	return freeGameID;	
}

void CServer::StopGameServer(unsigned int GameID, int MoveToGameID){
	sMap* pMap = m_pMaps;
	if(pMap){
		sGame* pGame = m_pGames;
		while (pGame->m_pNext) {
			if (pGame->m_pNext->m_uiGameID == GameID) break;
			pGame = pGame->m_pNext;
		}

		if (pMap->m_uiGameID == GameID) {
			m_pMaps = pMap->m_pNextMap;
			delete pMap;
		}
		else {
			while (pMap->m_pNextMap) {
				if (pMap->m_pNextMap->m_uiGameID == GameID) break;
				pMap = pMap->m_pNextMap;
			}
			if (!pMap->m_pNextMap) {
				delete m_pMaps;
				m_pMaps = 0;
			}
			else {
				sMap* mtmp = pMap->m_pNextMap->m_pNextMap;
				delete pMap->m_pNextMap;
				pMap->m_pNextMap = mtmp;
			}
		}
		
		if(pGame->m_pNext){	
			for(int c = 0; c < MAX_CLIENTS; c++)
			{
				if(m_aClients[c].m_State <= CClient::STATE_AUTH || m_aClients[c].m_uiGameID != pGame->m_pNext->m_uiGameID)
					continue;

				pGame->m_pNext->GameServer()->OnClientDrop(c, "", true);
				m_aClients[c].m_uiGameID = MoveToGameID == -1 ? 0 : MoveToGameID;
				SendMap(c, MoveToGameID == -1 ? 0 : MoveToGameID);
				m_aClients[c].Reset();
				m_aClients[c].m_State = CClient::STATE_CONNECTING;
			}

			delete pGame->m_pNext->m_pGameServer;
			sGame* pDeleteGame = pGame->m_pNext;
			pGame->m_pNext = pGame->m_pNext->m_pNext;
			delete pDeleteGame;
		}
	}
}

void CServer::MovePlayerToGameServer(int PlayerID, unsigned int GameID){
	if(PlayerID < MAX_CLIENTS){
		if(m_aClients[PlayerID].m_State <= CClient::STATE_AUTH || m_aClients[PlayerID].m_uiGameID == GameID)
			return;
		
		sGame* pGameLeave = m_pGames;
		while(pGameLeave){
			if(pGameLeave->m_uiGameID == m_aClients[PlayerID].m_uiGameID) break;
			pGameLeave = pGameLeave->m_pNext;
		}			
		
		
		sGame* pGame = GetGame(GameID);
		if (pGame) {
			if (pGame->m_uiGameID == GameID) {
				pGameLeave->GameServer()->OnClientDrop(PlayerID, "", true);

				m_aClients[PlayerID].m_uiGameID = GameID;
				SendMap(PlayerID, GameID);
				m_aClients[PlayerID].Reset();
				m_aClients[PlayerID].m_State = CClient::STATE_CONNECTING;
			}
		}
	}
}

void CServer::KickConnectingPlayers(unsigned int GameID, const char* pReason){
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(m_aClients[c].m_State != CClient::STATE_CONNECTING || m_aClients[c].m_uiGameID != GameID)
			continue;
		
		KickForce(c, pReason);
	}
}

bool CServer::CheckForConnectingPlayers(unsigned int GameID){	
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(m_aClients[c].m_State == CClient::STATE_CONNECTING && m_aClients[c].m_uiGameID == GameID)
			return true;
	}
	
	return false;
}

bool CServer::ChangeGameServerMap(unsigned int GameID, const char* pMapName){
	sGame* g = GetGame(GameID);
	if(g){
		if(ChangeMap(pMapName, GameID)){	
			int aPreferedTeams[MAX_CLIENTS];

			for(int c = 0; c < MAX_CLIENTS; c++)
			{
				aPreferedTeams[c] = GameServer()->PreferedTeamPlayer(c);
			}

			// new map loaded
			g->GameServer()->OnShutdown();

			for(int c = 0; c < MAX_CLIENTS; c++)
			{
				if(m_aClients[c].m_State <= CClient::STATE_AUTH || m_aClients[c].m_uiGameID != GameID)
					continue;

				m_aClients[c].m_uiGameID = GameID;
				SendMap(c, GameID);
				m_aClients[c].Reset();
				m_aClients[c].m_State = CClient::STATE_CONNECTING;
				m_aClients[c].m_PreferedTeam = aPreferedTeams[c];
			}
			
			sMap* pMap = m_pMaps;
			while(pMap){
				if(pMap->m_uiGameID == GameID) break;
				pMap = pMap->m_pNextMap;
			}
			
			if(pMap) g->GameServer()->OnInit(Kernel(), pMap->m_pMap, g->GameServer()->m_Config);
			
			return true;
		} else return false;
	} else return false;
}


sGame* CServer::GetGame(unsigned int GameID){
	sGame* p = m_pGames;
	while(p != NULL){
		if(p->m_uiGameID == GameID){
			return p;
		}
		p = p->m_pNext;
	}
	return NULL;
}

int CServer::SnapNewID()
{
	return m_IDPool.NewID();
}

void CServer::SnapFreeID(int ID)
{
	m_IDPool.FreeID(ID);
}


void *CServer::SnapNewItem(int Type, int ID, int Size)
{
	dbg_assert(Type >= 0 && Type <=0xffff, "incorrect type");
	dbg_assert(ID >= 0 && ID <=0xffff, "incorrect id");
	return ID < 0 ? 0 : m_SnapshotBuilder.NewItem(Type, ID, Size);
}

void CServer::SnapSetStaticsize(int ItemType, int Size)
{
	m_SnapshotDelta.SetStaticsize(ItemType, Size);
}

static CServer *CreateServer() { return new CServer(); }

int main(int argc, const char **argv) // ignore_convention
{
#if defined(CONF_FAMILY_WINDOWS)
	for(int i = 1; i < argc; i++) // ignore_convention
	{
		if(str_comp("-s", argv[i]) == 0 || str_comp("--silent", argv[i]) == 0) // ignore_convention
		{
			ShowWindow(GetConsoleWindow(), SW_HIDE);
			break;
		}
	}
#endif

	if(secure_random_init() != 0)
	{
		dbg_msg("secure", "could not initialize secure RNG");
		return -1;
	}

	CServer *pServer = CreateServer();
	IKernel *pKernel = IKernel::Create();

	// create the components
	IEngine *pEngine = CreateEngine("Teeworlds");
	IEngineMap *pEngineMap = CreateEngineMap();
	IGameServer *pGameServer = CreateGameServer();
	IConsole *pConsole = CreateConsole(CFGFLAG_SERVER|CFGFLAG_ECON);
	IEngineMasterServer *pEngineMasterServer = CreateEngineMasterServer();
	IStorage *pStorage = CreateStorage("Teeworlds", IStorage::STORAGETYPE_SERVER, argc, argv); // ignore_convention
	IConfig *pConfig = CreateConfig();

	pServer->InitRegister(&pServer->m_NetServer, pEngineMasterServer, pConsole);

	{
		bool RegisterFail = false;

		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pServer); // register as both
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pEngine);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IEngineMap*>(pEngineMap)); // register as both
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IMap*>(pEngineMap));
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pGameServer);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pConsole);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pStorage);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(pConfig);
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IEngineMasterServer*>(pEngineMasterServer)); // register as both
		RegisterFail = RegisterFail || !pKernel->RegisterInterface(static_cast<IMasterServer*>(pEngineMasterServer));

		if(RegisterFail)
			return -1;
	}

	pEngine->Init();
	pConfig->Init();
	pEngineMasterServer->Init();
	pEngineMasterServer->Load();

	// register all console commands
	pServer->RegisterCommands();

	// execute autoexec file
	pConsole->ExecuteFile("autoexec.cfg");

	// parse the command line arguments
	if(argc > 1) // ignore_convention
		pConsole->ParseArguments(argc-1, &argv[1]); // ignore_convention

	// restore empty config strings to their defaults
	pConfig->RestoreStrings();

	pEngine->InitLogfile();

	// run the server
	dbg_msg("server", "starting...");
	pServer->Run();

	// free
	delete pServer;
	delete pKernel;
	delete pEngineMap;
	delete pGameServer;
	delete pConsole;
	delete pEngineMasterServer;
	delete pStorage;
	delete pConfig;
	return 0;
}

static size_t WriteCallback(char *contents, size_t size, size_t nmemb, void *userp)
{
    strcat((char*)userp, contents);
    return size * nmemb;
}

bool CServer::IsProxy(const NETADDR *pAddr)
{
    char aAddrStr[NETADDR_MAXSTRSIZE];
    net_addr_str(pAddr, aAddrStr, sizeof(aAddrStr), false);

    char aUrl[256];
    str_format(aUrl, sizeof(aUrl), "https://proxycheck.io/v2/%s", aAddrStr);

    CURL *curl = curl_easy_init();
    if(!curl)
        return false;

    char aResponse[4096] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, aUrl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, aResponse);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK)
        return false;

    // Check for both status:ok and proxy:yes
    bool IsProxy = str_find_nocase(aResponse, "\"status\":\"ok\"") && 
                  str_find_nocase(aResponse, "\"proxy\":\"yes\"");

    if(IsProxy)
    {
        char aBuf[256];
        str_format(aBuf, sizeof(aBuf), "Proxy detected: %s", aAddrStr);
        Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "antiproxy", aBuf);
        
        if(g_Config.m_SvProxyCheckBan)
        {
            char aReason[128];
            str_format(aReason, sizeof(aReason), "Proxy/VPN detected");
            m_NetServer.NetBan()->BanAddr(pAddr, -1, aReason);
            
            str_format(aBuf, sizeof(aBuf), "Banned proxy: %s", aAddrStr);
            Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "antiproxy", aBuf);
        }
    }
    else
    {
        char aBuf[256];
        str_format(aBuf, sizeof(aBuf), "No proxy detected: %s", aAddrStr);
        Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "antiproxy", aBuf);
    }

    return IsProxy;
}