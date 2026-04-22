/*
* Copyright (C) 2009-2013 Ingmar Runge <ingmar@irsoft.de>
* Extended and fixed in 2026 by Lisko
*
* An ident server module for ZNC.
* https://github.com/KiNgMaR/znc/blob/msvc/win32/extra_modules/identserver.cpp
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License version 2 as published
* by the Free Software Foundation.
*/

#include "znc/znc.h"
#include "znc/ZNCString.h"
#include "znc/User.h"
#include "znc/IRCNetwork.h"
#include "znc/IRCSock.h"
#include "znc/Modules.h"
#include <map>
#include <set>
#include <ctime>

/************************************************************************/
/*   CLASS DECLARATIONS                                                 */
/************************************************************************/
class CIdentServer;

class CIdentServerMod : public CModule
{
protected:
	unsigned short m_serverPort;
	CIdentServer *m_identServer;
	bool m_listenFailed;
	CString m_sLastReply;
	CString m_sLastRequest;
	// Add a struct and map to track our network data
	struct IdentData {
		CString sReqTime;
		CString sRepTime;
		CString sRequest;
		CString sReply;
	};
	std::map<CIRCNetwork*, IdentData> m_mNetworkData;

public:
	MODCONSTRUCTOR(CIdentServerMod)
	{
		m_serverPort = 9113;
		m_identServer = NULL;
		m_listenFailed = false;

		AddHelpCommand();
		AddCommand("Status", "", "Displays status information about IdentServer", [this](const CString& sLine) {
			PrintStatus(); 
			});
	}

	virtual ~CIdentServerMod() {}

	void PrintStatus();
	bool OnLoad(const CString& sArgs, CString& sMessage) override;
	EModRet OnIRCConnecting(CIRCSock *pIRCSock) override;
	void OnIRCConnected() override;
	void OnIRCDisconnected() override; 
	void OnClientLogin() override;
	EModRet OnDeleteUser(CUser& User) override;
	EModRet OnDeleteNetwork(CIRCNetwork& Network) override;

	void NoLongerNeedsIdentServer();

	CIdentServer *GetIdentServer() { return m_identServer; }

	void SetLastReply(const CString& s) { m_sLastReply = s; };
	void SetLastRequest(const CString& s) { m_sLastRequest = s; };
	// Expose a new method to record the data
	void RecordNetworkIdentData(CIRCNetwork* pNetwork, const CString& sReqTime, const CString& sRepTime, const CString& sRequest, const CString& sReply) {
		m_mNetworkData[pNetwork] = {sReqTime, sRepTime, sRequest, sReply};
	}
	void PrintToServerBuffer(CIRCNetwork* pNet, const CString& sMessage);
};


class CIdentAcceptedSocket : public CSocket
{
public:
	CIdentAcceptedSocket(CModule *pMod);
	virtual ~CIdentAcceptedSocket();

	void ReadLine(const CS_STRING & sLine) override;
};


/**
* Ident server implementation.
* RFC 1413: http://www.faqs.org/rfcs/rfc1413.html
* Not thread safe!
**/
class CIdentServer : public CSocket
{
protected:
	std::set<CIRCNetwork*> m_activeUsers;
	CModule *m_pModule;
	unsigned short m_uPort;

	static bool AreIPStringsEqual(const CString& sIP1, const CString& sIP2);
public:
	CIdentServer(CModule *pMod, unsigned short uPort);
	virtual ~CIdentServer();

	bool IncreaseUseCount(CIRCNetwork* pUser);
	bool DecreaseUseCount(CIRCNetwork* pUser);
	bool InUse() { return !m_activeUsers.empty(); }
	bool StartListening();

	Csock *GetSockObj(const CS_STRING & sHostname, u_short uPort) override;
	bool ConnectionFrom(const CS_STRING & sHostname, u_short uPort) override;

	CString GetResponse(const CString& sLine, const CString& sSocketIP, const CString& sRemoteIP);
	const std::set<CIRCNetwork*>& GetActiveUsers() { return m_activeUsers; };
};


/************************************************************************/
/* CIdentServer method implementation section                           */
/************************************************************************/

CIdentServer::CIdentServer(CModule *pMod, unsigned short uPort) : CSocket(pMod)
{
	m_pModule = pMod;
	m_uPort = uPort;
}

bool CIdentServer::IncreaseUseCount(CIRCNetwork *pUser)
{
	if(m_activeUsers.find(pUser) != m_activeUsers.end())
	{
		return false;
	}

	m_activeUsers.insert(pUser);

	return true;
}

bool CIdentServer::DecreaseUseCount(CIRCNetwork *pUser)
{
	return (m_activeUsers.erase(pUser) != 0);
}

CString CIdentServer::GetResponse(const CString& sLine, const CString& sSocketIP, const CString& sRemoteIP)
{
	// 1. Capture Request Timestamp
	time_t tReq = time(NULL);
	char szReq[64];
	strftime(szReq, sizeof(szReq), "%Y-%m-%d %H:%M:%S", localtime(&tReq));

	unsigned short uLocalPort = 0; // local port that ZNC connected to IRC FROM
	unsigned short uRemotePort = 0; // remote server port that ZNC connected TO, e.g. 6667

	CString sResponseType = "ERROR";
	CString sAddInfo = "INVALID-PORT";
	
	// Keep track of which network we matched
	CIRCNetwork* pMatchedNetwork = NULL;

	DEBUG("IDENT request: " << sLine << " from " << sRemoteIP << " on " << sSocketIP);

	if(sscanf(sLine.c_str(), "%hu , %hu", &uLocalPort, &uRemotePort) == 2)
	{
		sAddInfo = "NO-USER";

		for(auto itu = CZNC::Get().GetUserMap().begin();
			itu != CZNC::Get().GetUserMap().end(); ++itu)
		{
			CUser* pUser = itu->second;
			bool bFound = false;

			for(CIRCNetwork* pNetwork : pUser->GetNetworks())
			{
				CIRCSock *pSock = pNetwork->GetIRCSock();

				if(!pSock)
					continue;

				DEBUG("Checking user (" << pSock->GetLocalPort() << ", " << pSock->GetRemotePort() << ", " << pSock->GetLocalIP() << ")");

				if(pSock->GetLocalPort() == uLocalPort &&
					pSock->GetRemotePort() == uRemotePort &&
					AreIPStringsEqual(pSock->GetLocalIP(), sSocketIP))
				{
					sResponseType = "USERID";
					sAddInfo = "UNIX : " + pNetwork->GetIdent();
					bFound = true;
					pMatchedNetwork = pNetwork; // Record the matched network!
					break;
				}

				DEBUG("Checking user fallback (" << pSock->GetRemoteIP() << ", " << pSock->GetRemotePort() << ", " << pSock->GetLocalIP() << ")");

				if(pSock->GetRemoteIP() == sRemoteIP &&
					pSock->GetRemotePort() == uRemotePort &&
					AreIPStringsEqual(pSock->GetLocalIP(), sSocketIP))
				{
					sResponseType = "USERID";
					sAddInfo = "UNIX : " + pNetwork->GetIdent();
					pMatchedNetwork = pNetwork; // Record the fallback match too!
				}
			}

			if(bFound)
				break;
		}
	}

	CString sReply = CString(uLocalPort) + ", " + CString(uRemotePort) + " : " + sResponseType + " : " + sAddInfo;

	DEBUG("IDENT response: " << sReply);

	// 2. Capture Reply Timestamp
	time_t tRep = time(NULL);
	char szRep[64];
	strftime(szRep, sizeof(szRep), "%Y-%m-%d %H:%M:%S", localtime(&tRep));

	CIdentServerMod *pMod = reinterpret_cast<CIdentServerMod*>(m_pModule);
	if(pMod)
	{
		// Clean the request line so it doesn't break ZNC's output formatting
		CString sCleanRequest = sLine.Replace_n("\r", "").Replace_n("\n", "");
		pMod->SetLastRequest(sCleanRequest + " from " + sRemoteIP + " on " + sSocketIP);
		pMod->SetLastReply(sReply);
		
		// Map the data to the network
		if (pMatchedNetwork) {
			pMod->RecordNetworkIdentData(pMatchedNetwork, szReq, szRep, sCleanRequest, sReply);
		}
	}

	return sReply;
}

bool CIdentServer::StartListening()
{
	return GetModule()->GetManager()->ListenAll(m_uPort, "IDENT_SERVER", false, SOMAXCONN, this);
}

Csock *CIdentServer::GetSockObj(const CS_STRING & sHostname, u_short uPort)
{
	return new CIdentAcceptedSocket(m_pModule);
}

bool CIdentServer::ConnectionFrom(const CS_STRING & sHostname, u_short uPort)
{
	DEBUG("IDENT connection from " << sHostname << ":" << uPort << " (on " << GetLocalIP() << ":" << GetLocalPort() << ")");

	return (!m_activeUsers.empty());
}

bool CIdentServer::AreIPStringsEqual(const CString& sIP1, const CString& sIP2)
{
	return sIP1.TrimPrefix_n("::ffff:").Equals(sIP2.TrimPrefix_n("::ffff:"));
}

CIdentServer::~CIdentServer()
{
}


/************************************************************************/
/* CIdentAcceptedSocket method implementation section                   */
/************************************************************************/

CIdentAcceptedSocket::CIdentAcceptedSocket(CModule *pMod) : CSocket(pMod)
{
	EnableReadLine();
}

void CIdentAcceptedSocket::ReadLine(const CS_STRING & sLine)
{
	CIdentServerMod *pMod = reinterpret_cast<CIdentServerMod*>(m_pModule);
	const CString sReply = pMod->GetIdentServer()->GetResponse(sLine, GetLocalIP(), GetRemoteIP());

	Write(sReply + "\r\n");

	Close(CLT_AFTERWRITE);
}

CIdentAcceptedSocket::~CIdentAcceptedSocket()
{
}


/************************************************************************/
/* CIdentServerMod method implementation section                        */
/************************************************************************/

bool CIdentServerMod::OnLoad(const CString& sArgs, CString& sMessage)
{
	if (!sArgs.empty())
	{
		unsigned short uPort = sArgs.Token(0).ToUShort();

		if (uPort > 0)
		{
			m_serverPort = uPort;
		}
		else
		{
			// If they typed letters or '0', refuse to load the module and warn them
			sMessage = "Invalid port number. Usage: loadmod identserver [port]";
			return false; 
		}
	}

	// If sArgs is empty, it simply keeps the default 9113 from the constructor.
	return true;
}

CIdentServerMod::EModRet CIdentServerMod::OnIRCConnecting(CIRCSock *pIRCSock)
{
	assert(m_pNetwork != NULL);

	DEBUG("CIdentServerMod::OnIRCConnecting");

	if(!m_identServer)
	{
		DEBUG("Starting up IDENT listener.");
		m_identServer = new CIdentServer(this, m_serverPort);

		if(!m_identServer->StartListening())
		{
			DEBUG("WARNING: Opening the listening socket failed!");
			m_listenFailed = true;
			m_identServer = NULL; /* Csock deleted the instance. (gross) */
			return CONTINUE;
		}
		else
		{
			m_listenFailed = false;
		}
	}

	m_identServer->IncreaseUseCount(m_pNetwork);

	return CONTINUE;
}

void CIdentServerMod::NoLongerNeedsIdentServer()
{
	assert(m_pNetwork != NULL);

	if(m_identServer)
	{
		m_identServer->DecreaseUseCount(m_pNetwork);

		if(!m_identServer->InUse())
		{
			DEBUG("Closing down IDENT listener.");
			m_identServer->Close();
			m_identServer = NULL;
		}
	}
}

void CIdentServerMod::PrintToServerBuffer(CIRCNetwork* pNet, const CString& sMessage)
{
	if (!pNet) return;

	CString sNick = pNet->GetCurNick();
	if (sNick.empty()) sNick = pNet->GetNick(); // Fallback

	CString sServerName = pNet->GetIRCServer();
	if (sServerName.empty()) sServerName = "IdentServer";

	// Formatting it as a NOTICE from the server pushes it to the server buffer
	PutUser(":" + sServerName + " NOTICE " + sNick + " :*** [IdentD] " + sMessage);
}

void CIdentServerMod::OnIRCConnected()
{
	if((!m_pClient) && (m_listenFailed))
	{
		PutModule("*** WARNING: Opening the listening socket failed!");
		PutModule("*** IDENT listener is NOT running.");
	}
	// Output our recorded data to the client if they exist for this network
	auto it = m_mNetworkData.find(m_pNetwork);
	if (it != m_mNetworkData.end()) {
		PrintToServerBuffer(m_pNetwork, "query received at " + it->second.sReqTime + " -> " + it->second.sRequest);
		PrintToServerBuffer(m_pNetwork, "reply sent at " + it->second.sRepTime + " -> " + it->second.sReply);
	}
	NoLongerNeedsIdentServer();
}

void CIdentServerMod::OnIRCDisconnected()
{
	// Clean up memory if !network
	m_mNetworkData.erase(m_pNetwork); 

	NoLongerNeedsIdentServer();
}

CIdentServerMod::EModRet CIdentServerMod::OnDeleteUser(CUser& User)
{
	// NoLongerNeedsIdentServer needs m_pNetwork, so we have to provide it:

	CIRCNetwork* pBackup = m_pNetwork;

	for(CIRCNetwork* pNetwork : User.GetNetworks())
	{
		m_pNetwork = pNetwork;

		NoLongerNeedsIdentServer();
	}

	m_pNetwork = pBackup;

	return CONTINUE;
}

CIdentServerMod::EModRet CIdentServerMod::OnDeleteNetwork(CIRCNetwork& Network)
{
	CIRCNetwork* pBackup = m_pNetwork;

	m_pNetwork = &Network; // meh

	// Clean up memory if !network
	m_mNetworkData.erase(m_pNetwork); 

	NoLongerNeedsIdentServer();

	m_pNetwork = pBackup;

	return CONTINUE;
}

void CIdentServerMod::OnClientLogin()
{
	if(m_listenFailed)
	{
		PutModule("*** WARNING: Opening the listening socket failed!");
		PutModule("*** IDENT listener is NOT running.");
	}
	if (m_pClient) 
	{
		CIRCNetwork* pNet = m_pClient->GetNetwork();
		if (pNet) 
		{
			auto it = m_mNetworkData.find(pNet);
			if (it != m_mNetworkData.end()) {
				PrintToServerBuffer(pNet, "query received at " + it->second.sReqTime + " -> " + it->second.sRequest);
				PrintToServerBuffer(pNet, "reply sent at " + it->second.sRepTime + " -> " + it->second.sReply);
			}
		}
	}
}

void CIdentServerMod::PrintStatus()
{
	if(m_identServer)
	{
		PutModule("IdentServer is listening on: " + m_identServer->GetLocalIP() + ":" + CString(m_serverPort));

		if(m_pUser->IsAdmin())
		{
			PutModule("List of active users/networks:");

			for(CIRCNetwork* pNetwork : m_identServer->GetActiveUsers())
			{
				PutModule("* " + pNetwork->GetUser()->GetCleanUserName() + "/" + pNetwork->GetName());
			}
		}
	}
	else
	{
		if(m_listenFailed)
		{
			PutModule("WARNING: Opening the listening socket failed!");
		}
		PutModule("IdentServer isn't listening.");
	}
	
	if(m_pUser->IsAdmin())
	{
		PutModule("Last IDENT request: " + m_sLastRequest);
		PutModule("Last IDENT reply: " + m_sLastReply);
	}
}

template <>
void TModInfo<CIdentServerMod>(CModInfo& Info) {
	Info.SetWikiPage("IdentServer");
	Info.SetHasArgs(true);
	Info.SetArgsHelpText("Enter custom listening port (e.g., 113). Default is 9113.");
}

GLOBALMODULEDEFS(CIdentServerMod, "Provides a simple IdentD server implementation. Default port: 9113")
