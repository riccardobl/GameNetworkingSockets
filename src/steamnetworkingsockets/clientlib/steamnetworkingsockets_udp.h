//====== Copyright Valve Corporation, All rights reserved. ====================

#ifndef STEAMNETWORKINGSOCKETS_UDP_H
#define STEAMNETWORKINGSOCKETS_UDP_H
#pragma once

#include "steamnetworkingsockets_connections.h"
#include <steamnetworkingsockets_messages_udp.pb.h>

namespace SteamNetworkingSocketsLib {

struct UDPSendPacketContext_t;

/////////////////////////////////////////////////////////////////////////////
//
// Listen socket used for direct IP connectivity
//
/////////////////////////////////////////////////////////////////////////////

class CSteamNetworkListenSocketDirectUDP : public CSteamNetworkListenSocketBase
{
public:
	CSteamNetworkListenSocketDirectUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkListenSocketDirectUDP();
	virtual bool APIGetAddress( SteamNetworkingIPAddr *pAddress ) override;

	/// Setup
	bool BInit( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

private:

	/// The socket we are bound to.  We own this socket.
	/// Any connections accepted through us become clients of this shared socket.
	CSharedSocket *m_pSock;

	/// Secret used to generate challenges
	uint8_t m_argbChallengeSecret[ 16 ];

	/// Generate a challenge
	uint64 GenerateChallenge( uint16 nTime, const netadr_t &adr ) const;

	// Callback to handle a packet when it doesn't match
	// any known address
	static void ReceivedFromUnknownHost( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketDirectUDP *pSock );

	// Process packets from a source address that does not already correspond to a session
	void Received_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow );
	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo );
	void SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo );
};

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

/// A connection over raw UDP
class CSteamNetworkConnectionUDP : public CSteamNetworkConnectionBase
{
public:
	CSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface );
	virtual ~CSteamNetworkConnectionUDP();

	virtual void FreeResources() override;

	/// Convenience wrapper to do the upcast, since we know what sort of
	/// listen socket we were connected on.
	inline CSteamNetworkListenSocketDirectUDP *ListenSocket() const { return assert_cast<CSteamNetworkListenSocketDirectUDP *>( m_pParentListenSocket ); }

	/// Implements CSteamNetworkConnectionBase
	virtual bool SendDataPacket( SteamNetworkingMicroseconds usecNow ) override;
	virtual int SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctx ) override;
	virtual EResult APIAcceptConnection() override;
	virtual bool BCanSendEndToEndConnectRequest() const override;
	virtual bool BCanSendEndToEndData() const override;
	virtual void SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow ) override;
	virtual void SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason ) override;
	virtual void ThinkConnection( SteamNetworkingMicroseconds usecNow ) override;
	virtual void GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const override;
	virtual EUnsignedCert AllowRemoteUnsignedCert() override;
	virtual EUnsignedCert AllowLocalUnsignedCert() override;

	/// Initiate a connection
	bool BInitConnect( const SteamNetworkingIPAddr &addressRemote, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg );

	/// Accept a connection that has passed the handshake phase
	bool BBeginAccept(
		CSteamNetworkListenSocketDirectUDP *pParent,
		const netadr_t &adrFrom,
		CSharedSocket *pSharedSock,
		const SteamNetworkingIdentity &identityRemote,
		uint32 unConnectionIDRemote,
		const CMsgSteamDatagramCertificateSigned &msgCert,
		const CMsgSteamDatagramSessionCryptInfoSigned &msgSessionInfo,
		SteamDatagramErrMsg &errMsg
	);

protected:

	/// Interface used to talk to the remote host
	IBoundUDPSocket *m_pSocket;

	// We need to customize our thinking to handle the connection state machine
	virtual void ConnectionStateChanged( ESteamNetworkingConnectionState eOldState ) override;

	static void PacketReceived( const void *pPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkConnectionUDP *pSelf );

	void Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow );
	void Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow );
	void Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow );
	void Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow );

	void SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg );
	void SendPacket( const void *pkt, int cbPkt );
	void SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal );

	void SendConnectOK( SteamNetworkingMicroseconds usecNow );
	void SendConnectionClosedOrNoConnection();
	void SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID );

	/// Process stats message, either inline or standalone
	void RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow );
	void SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason );
	void TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow );

	void PopulateSendPacketContext( UDPSendPacketContext_t &ctx, EStatsReplyRequest eReplyRequested );
};

/// A connection over loopback
class CSteamNetworkConnectionlocalhostLoopback final : public CSteamNetworkConnectionUDP
{
public:
	CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity );

	/// Setup two connections to be talking to each other
	static bool APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2], const SteamNetworkingIdentity pIdentity[2] );

	/// Base class overrides
	virtual void PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState ) override;
	virtual EUnsignedCert AllowRemoteUnsignedCert() override;
	virtual EUnsignedCert AllowLocalUnsignedCert() override;
};

} // namespace SteamNetworkingSocketsLib

#endif // STEAMNETWORKINGSOCKETS_UDP_H
