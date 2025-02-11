//====== Copyright Valve Corporation, All rights reserved. ====================

#include "steamnetworkingsockets_udp.h"
#include "csteamnetworkingsockets.h"
#include "crypto.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

const int k_cbSteamNetworkingMinPaddedPacketSize = 512;

// Put everything in a namespace, so we don't violate the one definition rule
namespace SteamNetworkingSocketsLib {

#pragma pack( push, 1 )

/// A protobuf-encoded message that is padded to ensure a minimum length
struct UDPPaddedMessageHdr
{
	uint8 m_nMsgID;
	uint16 m_nMsgLength;
};

struct UDPDataMsgHdr
{
	enum
	{
		kFlag_ProtobufBlob  = 0x01, // Protobuf-encoded message is inline (CMsgSteamSockets_UDP_Stats)
	};

	uint8 m_unMsgFlags;
	uint32 m_unToConnectionID; // Recipient's portion of the connection ID
	uint16 m_unSeqNum;

	// [optional, if flags&kFlag_ProtobufBlob]  varint-encoded protobuf blob size, followed by blob
	// Data frame(s)
	// End of packet
};
#pragma pack( pop )

const int k_nMaxRecentLocalConnectionIDs = 256;
static CUtlVectorFixed<uint16,k_nMaxRecentLocalConnectionIDs> s_vecRecentLocalConnectionIDs;

/////////////////////////////////////////////////////////////////////////////
//
// Packet parsing / handling utils
//
/////////////////////////////////////////////////////////////////////////////

bool BCheckRateLimitReportBadPacket( SteamNetworkingMicroseconds usecNow )
{
	static SteamNetworkingMicroseconds s_usecLastReport;
	if ( s_usecLastReport + k_nMillion*2 > usecNow )
		return false;
	s_usecLastReport = usecNow;
	return true;
}

void ReallyReportBadPacket( const netadr_t &adrFrom, const char *pszMsgType, const char *pszFmt, ... )
{
	char buf[ 2048 ];
	va_list ap;
	va_start( ap, pszFmt );
	V_vsprintf_safe( buf, pszFmt, ap );
	va_end( ap );
	V_StripTrailingWhitespaceASCII( buf );

	if ( !pszMsgType || !pszMsgType[0] )
		pszMsgType = "message";

	SpewMsg( "Ignored bad %s from %s.  %s\n", pszMsgType, CUtlNetAdrRender( adrFrom ).String(), buf );
}

#define ReportBadPacketFrom( adrFrom, pszMsgType, /* fmt */ ... ) \
	( BCheckRateLimitReportBadPacket( usecNow ) ? ReallyReportBadPacket( adrFrom, pszMsgType, __VA_ARGS__ ) : (void)0 )

#define ReportBadPacket( pszMsgType, /* fmt */ ... ) \
	ReportBadPacketFrom( adrFrom, pszMsgType, __VA_ARGS__ )


#define ParseProtobufBody( pvMsg, cbMsg, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	if ( !msgVar.ParseFromArray( pvMsg, cbMsg ) ) \
	{ \
		ReportBadPacket( # CMsgCls, "Protobuf parse failed." ); \
		return; \
	}

#define ParsePaddedPacket( pvPkt, cbPkt, CMsgCls, msgVar ) \
	CMsgCls msgVar; \
	{ \
		if ( cbPkt < k_cbSteamNetworkingMinPaddedPacketSize ) \
		{ \
			ReportBadPacket( # CMsgCls, "Packet is %d bytes, must be padded to at least %d bytes.", cbPkt, k_cbSteamNetworkingMinPaddedPacketSize ); \
			return; \
		} \
		const UDPPaddedMessageHdr *hdr = static_cast< const UDPPaddedMessageHdr * >( pvPkt ); \
		int nMsgLength = LittleWord( hdr->m_nMsgLength ); \
		if ( nMsgLength <= 0 || int(nMsgLength+sizeof(UDPPaddedMessageHdr)) > cbPkt ) \
		{ \
			ReportBadPacket( # CMsgCls, "Invalid encoded message length %d.  Packet is %d bytes.", nMsgLength, cbPkt ); \
			return; \
		} \
		if ( !msgVar.ParseFromArray( hdr+1, nMsgLength ) ) \
		{ \
			ReportBadPacket( # CMsgCls, "Protobuf parse failed." ); \
			return; \
		} \
	}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketDirectUDP
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkListenSocketDirectUDP::CSteamNetworkListenSocketDirectUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkListenSocketBase( pSteamNetworkingSocketsInterface )
{
	m_pSock = nullptr;
}

CSteamNetworkListenSocketDirectUDP::~CSteamNetworkListenSocketDirectUDP()
{
	// Clean up socket, if any
	if ( m_pSock )
	{
		delete m_pSock;
		m_pSock = nullptr;
	}
}

bool CSteamNetworkListenSocketDirectUDP::BInit( const SteamNetworkingIPAddr &localAddr, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	Assert( m_pSock == nullptr );

	if ( localAddr.m_port == 0 )
	{
		V_strcpy_safe( errMsg, "Must specify local port." );
		return false;
	}

	// Set options, add us to the global table
	if ( !BInitListenSocketCommon( nOptions, pOptions, errMsg ) )
		return false;

	m_pSock = new CSharedSocket;
	if ( !m_pSock->BInit( localAddr, CRecvPacketCallback( ReceivedFromUnknownHost, this ), errMsg ) )
	{
		delete m_pSock;
		m_pSock = nullptr;
		return false;
	}

	CCrypto::GenerateRandomBlock( m_argbChallengeSecret, sizeof(m_argbChallengeSecret) );

	return true;
}

bool CSteamNetworkListenSocketDirectUDP::APIGetAddress( SteamNetworkingIPAddr *pAddress )
{
	if ( !m_pSock )
	{
		Assert( false );
		return false;
	}

	const SteamNetworkingIPAddr *pBoundAddr = m_pSock->GetBoundAddr();
	if ( !pBoundAddr )
		return false;
	if ( pAddress )
		*pAddress = *pBoundAddr;
	return true;
}

/////////////////////////////////////////////////////////////////////////////
//
// CSteamNetworkListenSocketUDP packet handling
//
/////////////////////////////////////////////////////////////////////////////

void CSteamNetworkListenSocketDirectUDP::ReceivedFromUnknownHost( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkListenSocketDirectUDP *pSock )
{
	const uint8 *pPkt = static_cast<const uint8 *>( pvPkt );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	if ( cbPkt < 5 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	if ( *pPkt & 0x80 )
	{
		if ( *(uint32*)pPkt == 0xffffffff )
		{
			// Source engine connectionless packet (LAN discovery, etc).
			// Just ignore it, and don't even spew.
		}
		else
		{
			// A stray data packet.  Just ignore it.
			//
			// When clients are able to actually establish a connection, after that connection
			// is over we will use the FinWait state to close down the connection gracefully.
			// But since we don't have that connection in our table anymore, either this guy
			// never had a connection, or else we believe he knows that the connection was closed,
			// or the FinWait state has timed out.
			ReportBadPacket( "Data", "Stray data packet from host with no connection.  Ignoring." );
		}
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeRequest )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ChallengeRequest, msg )
		pSock->Received_ChallengeRequest( msg, adrFrom, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectRequest )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectRequest, msg )
		pSock->Received_ConnectRequest( msg, adrFrom, cbPkt, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		pSock->Received_ConnectionClosed( msg, adrFrom, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		// They don't think there's a connection on this address.
		// We agree -- connection ID doesn't matter.  Nothing else to do.
	}
	else
	{
		// Any other lead byte is bogus
		//
		// Note in particular that these packet types should be ignored:
		//
		// k_ESteamNetworkingUDPMsg_ChallengeReply
		// k_ESteamNetworkingUDPMsg_ConnectOK
		//
		// We are not initiating connections, so we shouldn't ever get
		// those sorts of replies.

		ReportBadPacket( "packet", "Invalid lead byte 0x%02x", *pPkt );
	}
}

uint64 CSteamNetworkListenSocketDirectUDP::GenerateChallenge( uint16 nTime, const netadr_t &adr ) const
{
	#pragma pack(push,1)
	struct
	{
		uint16 nTime;
		uint16 nPort;
		uint8 ipv6[16];
	} data;
	#pragma pack(pop)
	data.nTime = nTime;
	data.nPort = adr.GetPort();
	adr.GetIPV6( data.ipv6 );
	uint64 nChallenge = siphash( (const uint8_t *)&data, sizeof(data), m_argbChallengeSecret );
	return ( nChallenge & 0xffffffffffff0000ull ) | nTime;
}

inline uint16 GetChallengeTime( SteamNetworkingMicroseconds usecNow )
{
	return uint16( usecNow >> 20 );
}

void CSteamNetworkListenSocketDirectUDP::Received_ChallengeRequest( const CMsgSteamSockets_UDP_ChallengeRequest &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
{
	if ( msg.connection_id() == 0 )
	{
		ReportBadPacket( "ChallengeRequest", "Missing connection_id." );
		return;
	}
	//CSteamID steamIDClient( uint64( msg.client_steam_id() ) );
	//if ( !steamIDClient.IsValid() )
	//{
	//	ReportBadPacket( "ChallengeRequest", "Missing/invalid SteamID.", cbPkt );
	//	return;
	//}

	// Get time value of challenge
	uint16 nTime = GetChallengeTime( usecNow );

	// Generate a challenge
	uint64 nChallenge = GenerateChallenge( nTime, adrFrom );

	// Send them a reply
	CMsgSteamSockets_UDP_ChallengeReply msgReply;
	msgReply.set_connection_id( msg.connection_id() );
	msgReply.set_challenge( nChallenge );
	msgReply.set_your_timestamp( msg.my_timestamp() );
	msgReply.set_protocol_version( k_nCurrentProtocolVersion );
	SendMsg( k_ESteamNetworkingUDPMsg_ChallengeReply, msgReply, adrFrom );
}

void CSteamNetworkListenSocketDirectUDP::Received_ConnectRequest( const CMsgSteamSockets_UDP_ConnectRequest &msg, const netadr_t &adrFrom, int cbPkt, SteamNetworkingMicroseconds usecNow )
{
	SteamDatagramErrMsg errMsg;

	// Make sure challenge was generated relatively recently
	uint16 nTimeThen = uint32( msg.challenge() );
	uint16 nElapsed = GetChallengeTime( usecNow ) - nTimeThen;
	if ( nElapsed > GetChallengeTime( 4*k_nMillion ) )
	{
		ReportBadPacket( "ConnectRequest", "Challenge too old." );
		return;
	}

	// Assuming we sent them this time value, re-create the challenge we would have sent them.
	if ( GenerateChallenge( nTimeThen, adrFrom ) != msg.challenge() )
	{
		ReportBadPacket( "ConnectRequest", "Incorrect challenge.  Could be spoofed." );
		return;
	}

	uint32 unClientConnectionID = msg.client_connection_id();
	if ( unClientConnectionID == 0 )
	{
		ReportBadPacket( "ConnectRequest", "Missing connection ID" );
		return;
	}

	// Parse out identity from the cert
	SteamNetworkingIdentity identityRemote;
	bool bIdentityInCert = true;
	{
		// !SPEED! We are deserializing the cert here,
		// and then we are going to do it again below.
		// Should refactor to fix this.
		int r = SteamNetworkingIdentityFromSignedCert( identityRemote, msg.cert(), errMsg );
		if ( r < 0 )
		{
			ReportBadPacket( "ConnectRequest", "Bad identity in cert.  %s", errMsg );
			return;
		}
		if ( r == 0 )
		{
			// No identity in the cert.  Check if they put it directly in the connect message
			bIdentityInCert = false;
			r = SteamNetworkingIdentityFromProtobuf( identityRemote, msg, identity_string, legacy_identity_binary, legacy_client_steam_id, errMsg );
			if ( r < 0 )
			{
				ReportBadPacket( "ConnectRequest", "Bad identity.  %s", errMsg );
				return;
			}
			if ( r == 0 )
			{
				// If no identity was presented, it's the same as them saying they are "localhost"
				identityRemote.SetLocalHost();
			}
		}
	}
	Assert( !identityRemote.IsInvalid() );

	// Check if they are using an IP address as an identity (possibly the anonymous "localhost" identity)
	if ( identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress )
	{
		SteamNetworkingIPAddr addr;
		adrFrom.GetIPV6( addr.m_ipv6 );
		addr.m_port = adrFrom.GetPort();

		if ( identityRemote.IsLocalHost() )
		{
			if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectRequest", "Unauthenticated connections not allowed." );
				return;
			}

			// Set their identity to their real address (including port)
			identityRemote.SetIPAddr( addr );
		}
		else
		{
			// FIXME - Should the address be required to match?
			// If we are behind NAT, it won't.
			//if ( memcmp( addr.m_ipv6, identityRemote.m_ip.m_ipv6, sizeof(addr.m_ipv6) ) != 0
			//	|| ( identityRemote.m_ip.m_port != 0 && identityRemote.m_ip.m_port != addr.m_port ) ) // Allow 0 port in the identity to mean "any port"
			//{
			//	ReportBadPacket( "ConnectRequest", "Identity in request is %s, but packet is coming from %s." );
			//	return;
			//}

			// It's not really clear what the use case is here for
			// requesting a specific IP address as your identity,
			// and not using localhost.  If they have a cert, assume it's
			// meaningful.  Remember: the cert could be unsigned!  That
			// is a separate issue which will be handled later, whether
			// we want to allow that.
			if ( !bIdentityInCert )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectRequest", "Cannot use specific IP address." );
				return;
			}
		}
	}

	// Does this connection already exist?  (At a different address?)
	int h = m_mapChildConnections.Find( RemoteConnectionKey_t{ identityRemote, unClientConnectionID } );
	if ( h != m_mapChildConnections.InvalidIndex() )
	{
		CSteamNetworkConnectionBase *pOldConn = m_mapChildConnections[ h ];
		Assert( pOldConn->m_identityRemote == identityRemote );
		Assert( pOldConn->GetRemoteAddr() != adrFrom ); // or else why didn't we already map it directly to them!

		// NOTE: We cannot just destroy the object.  The API semantics
		// are that all connections, once accepted and made visible
		// to the API, must be closed by the application.
		ReportBadPacket( "ConnectRequest", "Rejecting connection request from %s at %s, connection ID %u.  That steamID/ConnectionID pair already has a connection from %s\n",
			SteamNetworkingIdentityRender( identityRemote ).c_str(), CUtlNetAdrRender( adrFrom ).String(), unClientConnectionID, CUtlNetAdrRender( pOldConn->GetRemoteAddr() ).String()
		);

		CMsgSteamSockets_UDP_ConnectionClosed msgReply;
		msgReply.set_to_connection_id( unClientConnectionID );
		msgReply.set_reason_code( k_ESteamNetConnectionEnd_Misc_Generic );
		msgReply.set_debug( "A connection with that ID already exists." );
		SendPaddedMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msgReply, adrFrom );
		return;
	}

	CSteamNetworkConnectionUDP *pConn = new CSteamNetworkConnectionUDP( m_pSteamNetworkingSocketsInterface );

	// OK, they have completed the handshake.  Accept the connection.
	if ( !pConn->BBeginAccept( this, adrFrom, m_pSock, identityRemote, unClientConnectionID, msg.cert(), msg.crypt(), errMsg ) )
	{
		SpewWarning( "Failed to accept connection from %s.  %s\n", CUtlNetAdrRender( adrFrom ).String(), errMsg );
		pConn->Destroy();
		return;
	}

	pConn->m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	// Did they send us a ping estimate?
	if ( msg.has_ping_est_ms() )
	{
		if ( msg.ping_est_ms() > 1500 )
		{
			SpewWarning( "[%s] Ignoring really large ping estimate %u in connect request", pConn->GetDescription(), msg.has_ping_est_ms() );
		}
		else
		{
			pConn->m_statsEndToEnd.m_ping.ReceivedPing( msg.ping_est_ms(), usecNow );
		}
	}

	// Save of timestamp that we will use to reply to them when the application
	// decides to accept the connection
	if ( msg.has_my_timestamp() )
	{
		pConn->m_ulHandshakeRemoteTimestamp = msg.my_timestamp();
		pConn->m_usecWhenReceivedHandshakeRemoteTimestamp = usecNow;
	}
}

void CSteamNetworkListenSocketDirectUDP::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, const netadr_t &adrFrom, SteamNetworkingMicroseconds usecNow )
{
	// Send an ack.  Note that we require the inbound message to be padded
	// to a minimum size, and this reply is tiny, so we are not at a risk of
	// being used for reflection, even though the source address could be spoofed.
	CMsgSteamSockets_UDP_NoConnection msgReply;
	if ( msg.from_connection_id() )
		msgReply.set_to_connection_id( msg.from_connection_id() );
	if ( msg.to_connection_id() )
		msgReply.set_from_connection_id( msg.to_connection_id() );
	SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msgReply, adrFrom );
}

void CSteamNetworkListenSocketDirectUDP::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t &adrTo )
{
	if ( !m_pSock )
	{
		Assert( false );
		return;
	}

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = msg.ByteSize()+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), cbPkt, (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	// Send the reply
	m_pSock->BSendRawPacket( pkt, cbPkt, adrTo );
}

void CSteamNetworkListenSocketDirectUDP::SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg, const netadr_t adrTo )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = msg.ByteSize();
	hdr->m_nMsgID = nMsgID;
	hdr->m_nMsgLength = LittleWord( uint16( nMsgLength ) );
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt + sizeof(*hdr) );
	int cbPkt = pEnd - pkt;
	Assert( cbPkt == int( sizeof(*hdr) + nMsgLength ) );
	cbPkt = MAX( cbPkt, k_cbSteamNetworkingMinPaddedPacketSize );

	m_pSock->BSendRawPacket( pkt, cbPkt, adrTo );
}

/////////////////////////////////////////////////////////////////////////////
//
// IP connections
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionUDP::CSteamNetworkConnectionUDP( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface )
: CSteamNetworkConnectionBase( pSteamNetworkingSocketsInterface )
{
	m_pSocket = nullptr;
}

CSteamNetworkConnectionUDP::~CSteamNetworkConnectionUDP()
{
	AssertMsg( !m_pSocket, "Connection not destroyed properly" );
}

void CSteamNetworkConnectionUDP::GetConnectionTypeDescription( ConnectionTypeDescription_t &szDescription ) const
{
	char szAddr[ 64 ];
	if ( m_pSocket )
	{
		SteamNetworkingIPAddr adrRemote;
		NetAdrToSteamNetworkingIPAddr( adrRemote, m_pSocket->GetRemoteHostAddr() );
		adrRemote.ToString( szAddr, sizeof(szAddr), true );
		if (
			m_identityRemote.IsLocalHost()
			|| ( m_identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress && adrRemote == m_identityRemote.m_ip )
		) {
			V_sprintf_safe( szDescription, "UDP %s", szAddr );
			return;
		}
	}
	else
	{
		V_strcpy_safe( szAddr, "???" );
	}

	SteamNetworkingIdentityRender sIdentity( m_identityRemote );

	V_sprintf_safe( szDescription, "UDP %s@%s", sIdentity.c_str(), szAddr );
}

void CSteamNetworkConnectionUDP::FreeResources()
{
	if ( m_pSocket )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
	}

	// Base class cleanup
	CSteamNetworkConnectionBase::FreeResources();
}

template<>
inline uint32 StatsMsgImpliedFlags<CMsgSteamSockets_UDP_Stats>( const CMsgSteamSockets_UDP_Stats &msg )
{
	return msg.has_stats() ? msg.ACK_REQUEST_E2E : 0;
}

struct UDPSendPacketContext_t : SendPacketContext<CMsgSteamSockets_UDP_Stats>
{
	inline explicit UDPSendPacketContext_t( SteamNetworkingMicroseconds usecNow, const char *pszReason ) : SendPacketContext<CMsgSteamSockets_UDP_Stats>( usecNow, pszReason ) {}
	int m_nStatsNeed;
};


void CSteamNetworkConnectionUDP::PopulateSendPacketContext( UDPSendPacketContext_t &ctx, EStatsReplyRequest eReplyRequested )
{
	SteamNetworkingMicroseconds usecNow = ctx.m_usecNow;

	// What effective flags should we send
	uint32 nFlags = 0;
	int nReadyToSendTracer = 0;
	if ( eReplyRequested == k_EStatsReplyRequest_Immediate || m_statsEndToEnd.BNeedToSendPingImmediate( usecNow ) )
		nFlags |= ctx.msg.ACK_REQUEST_E2E | ctx.msg.ACK_REQUEST_IMMEDIATE;
	else if ( eReplyRequested == k_EStatsReplyRequest_DelayedOK || m_statsEndToEnd.BNeedToSendKeepalive( usecNow ) )
		nFlags |= ctx.msg.ACK_REQUEST_E2E;
	else
	{
		nReadyToSendTracer = m_statsEndToEnd.ReadyToSendTracerPing( usecNow );
		if ( nReadyToSendTracer > 1 )
			nFlags |= ctx.msg.ACK_REQUEST_E2E;
	}

	ctx.m_nFlags = nFlags;

	// Need to send any connection stats stats?
	if ( m_statsEndToEnd.BNeedToSendStats( usecNow ) )
	{
		ctx.m_nStatsNeed = 2;
		m_statsEndToEnd.PopulateMessage( *ctx.msg.mutable_stats(), usecNow );

		if ( nReadyToSendTracer > 0 )
			nFlags |= ctx.msg.ACK_REQUEST_E2E;

		ctx.SlamFlagsAndCalcSize();
		ctx.CalcMaxEncryptedPayloadSize( sizeof(UDPDataMsgHdr), this );
	}
	else
	{
		// Populate flags now, based on what is implied from what we HAVE to send
		ctx.SlamFlagsAndCalcSize();
		ctx.CalcMaxEncryptedPayloadSize( sizeof(UDPDataMsgHdr), this );

		// Would we like to try to send some additional stats, if there is room?
		if ( m_statsEndToEnd.BReadyToSendStats( usecNow ) )
		{
			if ( nReadyToSendTracer > 0 )
				nFlags |= ctx.msg.ACK_REQUEST_E2E;
			m_statsEndToEnd.PopulateMessage( *ctx.msg.mutable_stats(), usecNow );
			ctx.SlamFlagsAndCalcSize();
			ctx.m_nStatsNeed = 1;
		}
		else
		{
			// No need to send any stats right now
			ctx.m_nStatsNeed = 0;
		}
	}
}

void CSteamNetworkConnectionUDP::SendStatsMsg( EStatsReplyRequest eReplyRequested, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	UDPSendPacketContext_t ctx( usecNow, pszReason );
	PopulateSendPacketContext( ctx, eReplyRequested );

	// Send a data packet (maybe containing ordinary data), with this piggy backed on top of it
	SNP_SendPacket( ctx );
}

bool CSteamNetworkConnectionUDP::SendDataPacket( SteamNetworkingMicroseconds usecNow )
{
	// Populate context struct with any stats we want/need to send, and how much space we need to reserve for it
	UDPSendPacketContext_t ctx( usecNow, "data" );
	PopulateSendPacketContext( ctx, k_EStatsReplyRequest_NothingToSend );

	// Send a packet
	return SNP_SendPacket( ctx );
}

int CSteamNetworkConnectionUDP::SendEncryptedDataChunk( const void *pChunk, int cbChunk, SendPacketContext_t &ctxBase )
{
	if ( !m_pSocket )
	{
		Assert( false );
		return 0;
	}

	UDPSendPacketContext_t &ctx = static_cast<UDPSendPacketContext_t &>( ctxBase );

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	UDPDataMsgHdr *hdr = (UDPDataMsgHdr *)pkt;
	hdr->m_unMsgFlags = 0x80;
	Assert( m_unConnectionIDRemote != 0 );
	hdr->m_unToConnectionID = LittleDWord( m_unConnectionIDRemote );
	hdr->m_unSeqNum = LittleWord( m_statsEndToEnd.ConsumeSendPacketNumberAndGetWireFmt( ctx.m_usecNow ) );

	byte *p = (byte*)( hdr + 1 );

	// Check how much bigger we could grow the header
	// and still fit in a packet
	int cbHdrOutSpaceRemaining = pkt + sizeof(pkt) - p - cbChunk;
	if ( cbHdrOutSpaceRemaining < 0 )
	{
		AssertMsg( false, "MTU / header size problem!" );
		return 0;
	}

	// Try to trim stuff from blob, if it won't fit
	while ( ctx.m_cbTotalSize > cbHdrOutSpaceRemaining )
	{

		if ( ctx.msg.has_stats() )
		{
			AssertMsg( ctx.m_nStatsNeed == 1, "We didn't reserve enough space for stats!" );
			if ( ctx.msg.stats().has_instantaneous() && ctx.msg.stats().has_lifetime() )
			{
				// Trying to send both - clear instantaneous
				ctx.msg.mutable_stats()->clear_instantaneous();
			}
			else
			{
				// Trying to send just one or the other.  Clear the whole container.
				ctx.msg.clear_stats();
			}

			ctx.SlamFlagsAndCalcSize();
			continue;
		}

		// Nothing left to clear!?  We shouldn't get here!
		AssertMsg( false, "Serialized stats message still won't fit, ever after clearing everything?" );
		ctx.m_cbTotalSize = 0;
		break;
	}

	if ( ctx.Serialize( p ) )
	{
		// Update bookkeeping with the stuff we are actually sending
		TrackSentStats( ctx.msg, true, ctx.m_usecNow );

		// Mark header with the flag
		hdr->m_unMsgFlags |= hdr->kFlag_ProtobufBlob;
	}

	// !FIXME! Time since previous, for jitter measurement?

	// Use gather-based send.  This saves one memcpy of every payload
	iovec gather[2];
	gather[0].iov_base = pkt;
	gather[0].iov_len = p - pkt;
	gather[1].iov_base = const_cast<void*>( pChunk );
	gather[1].iov_len = cbChunk;

	int cbSend = gather[0].iov_len + gather[1].iov_len;
	Assert( cbSend <= sizeof(pkt) ); // Bug in the code above.  We should never "overflow" the packet.  (Ignoring the fact that we using a gather-based send.  The data could be tiny with a large header for piggy-backed stats.)

	// !FIXME! Should we track data payload separately?  Maybe we ought to track
	// *messages* instead of packets.

	// Send it
	SendPacketGather( 2, gather, cbSend );
	return cbSend;
}

bool CSteamNetworkConnectionUDP::BInitConnect( const SteamNetworkingIPAddr &addressRemote, int nOptions, const SteamNetworkingConfigValue_t *pOptions, SteamDatagramErrMsg &errMsg )
{
	AssertMsg( !m_pSocket, "Trying to connect when we already have a socket?" );

	// We're initiating a connection, not being accepted on a listen socket
	Assert( !m_pParentListenSocket );

	netadr_t netadrRemote;
	SteamNetworkingIPAddrToNetAdr( netadrRemote, addressRemote );

	// For now we're just assuming each connection will gets its own socket,
	// on an ephemeral port.  Later we could add a setting to enable
	// sharing of the socket.
	m_pSocket = OpenUDPSocketBoundToHost( netadrRemote, CRecvPacketCallback( PacketReceived, this ), errMsg );
	if ( !m_pSocket )
		return false;

	// We use identity validity to denote when our connection has been accepted,
	// so it's important that it be cleared.  (It should already be so.)
	Assert( m_identityRemote.IsInvalid() );
	m_identityRemote.Clear();

	// We just opened a socket aiming at this address, so we know what the remote addr will be.
	m_netAdrRemote = netadrRemote;

	// We should know our own identity, unless the app has said it's OK to go without this.
	if ( m_identityLocal.IsInvalid() ) // Specific identity hasn't already been set (by derived class, etc)
	{

		// Use identity from the interface, if we have one
		m_identityLocal = m_pSteamNetworkingSocketsInterface->InternalGetIdentity();
		if ( m_identityLocal.IsInvalid())
		{

			// We don't know who we are.  Should we attempt anonymous?
			if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				V_strcpy_safe( errMsg, "Unable to determine local identity, and auth required.  Not logged in?" );
				return false;
			}

			m_identityLocal.SetLocalHost();
		}
	}

	// Let base class do some common initialization
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, nOptions, pOptions, errMsg ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		return false;
	}

	// Start the connection state machine, and send the first request packet.
	CheckConnectionStateAndSetNextThinkTime( usecNow );

	return true;
}

bool CSteamNetworkConnectionUDP::BCanSendEndToEndConnectRequest() const
{
	return m_pSocket != nullptr;
}

bool CSteamNetworkConnectionUDP::BCanSendEndToEndData() const
{
	return m_pSocket != nullptr;
}

void CSteamNetworkConnectionUDP::SendEndToEndConnectRequest( SteamNetworkingMicroseconds usecNow )
{
	Assert( !m_pParentListenSocket );
	Assert( GetState() == k_ESteamNetworkingConnectionState_Connecting ); // Why else would we be doing this?
	Assert( m_unConnectionIDLocal );

	CMsgSteamSockets_UDP_ChallengeRequest msg;
	msg.set_connection_id( m_unConnectionIDLocal );
	//msg.set_client_steam_id( m_steamIDLocal.ConvertToUint64() );
	msg.set_my_timestamp( usecNow );
	msg.set_protocol_version( k_nCurrentProtocolVersion );

	// Send it, with padding
	SendPaddedMsg( k_ESteamNetworkingUDPMsg_ChallengeRequest, msg );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

void CSteamNetworkConnectionUDP::SendEndToEndStatsMsg( EStatsReplyRequest eRequest, SteamNetworkingMicroseconds usecNow, const char *pszReason )
{
	SendStatsMsg( eRequest, usecNow, pszReason );
}

void CSteamNetworkConnectionUDP::ThinkConnection( SteamNetworkingMicroseconds usecNow )
{

	// FIXME - We should refactor this, maybe promote this to the base class.
	//         There's really nothing specific to plain UDP transport here.

	// Check if we have stats we need to flush out
	if ( !m_statsEndToEnd.IsDisconnected() )
	{

		// Do we need to send something immediately, for any reason?
		const char *pszReason = NeedToSendEndToEndStatsOrAcks( usecNow );
		if ( pszReason )
		{
			SendStatsMsg( k_EStatsReplyRequest_NothingToSend, usecNow, pszReason );

			// Make sure that took care of what we needed!

			Assert( !NeedToSendEndToEndStatsOrAcks( usecNow ) );
		}

		// Make sure we are scheduled to think the next time we need to
		SteamNetworkingMicroseconds usecNextStatsThink = m_statsEndToEnd.GetNextThinkTime( usecNow );
		if ( usecNextStatsThink <= usecNow )
		{
			AssertMsg( false, "We didn't send all the stats we needed to!" );
		}
		else
		{
			EnsureMinThinkTime( usecNextStatsThink );
		}
	}
}

bool CSteamNetworkConnectionUDP::BBeginAccept(
	CSteamNetworkListenSocketDirectUDP *pParent,
	const netadr_t &adrFrom,
	CSharedSocket *pSharedSock,
	const SteamNetworkingIdentity &identityRemote,
	uint32 unConnectionIDRemote,
	const CMsgSteamDatagramCertificateSigned &msgCert,
	const CMsgSteamDatagramSessionCryptInfoSigned &msgCryptSessionInfo,
	SteamDatagramErrMsg &errMsg
)
{
	AssertMsg( !m_pSocket, "Trying to accept when we already have a socket?" );

	// Get an interface just to talk just to this guy
	m_pSocket = pSharedSock->AddRemoteHost( adrFrom, CRecvPacketCallback( PacketReceived, this ) );
	if ( !m_pSocket )
	{
		V_strcpy_safe( errMsg, "Unable to create a bound socket on the shared socket." );
		return false;
	}

	m_identityRemote = identityRemote;

	// Caller should have ensured a valid identity
	Assert( !m_identityRemote.IsInvalid() );

	m_unConnectionIDRemote = unConnectionIDRemote;
	m_netAdrRemote = adrFrom;
	pParent->AddChildConnection( this );

	// Let base class do some common initialization
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();
	if ( !CSteamNetworkConnectionBase::BInitConnection( usecNow, 0, nullptr, errMsg ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		return false;
	}

	// Process crypto handshake now
	if ( !BRecvCryptoHandshake( msgCert, msgCryptSessionInfo, true ) )
	{
		m_pSocket->Close();
		m_pSocket = nullptr;
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		V_sprintf_safe( errMsg, "Failed crypto init.  %s", m_szEndDebug );
		return false;
	}

	// OK
	return true;
}

EResult CSteamNetworkConnectionUDP::APIAcceptConnection()
{
	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Send the message
	SendConnectOK( usecNow );

	// We are fully connected
	ConnectionState_Connected( usecNow );

	// OK
	return k_EResultOK;
}

void CSteamNetworkConnectionUDP::SendMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	pkt[0] = nMsgID;
	int cbPkt = msg.ByteSize()+1;
	if ( cbPkt > sizeof(pkt) )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), cbPkt, (int)sizeof(pkt) );
		return;
	}
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt+1 );
	Assert( cbPkt == pEnd - pkt );

	SendPacket( pkt, cbPkt );
}

void CSteamNetworkConnectionUDP::SendPaddedMsg( uint8 nMsgID, const google::protobuf::MessageLite &msg )
{

	uint8 pkt[ k_cbSteamNetworkingSocketsMaxUDPMsgLen ];
	V_memset( pkt, 0, sizeof(pkt) ); // don't send random bits from our process memory over the wire!
	UDPPaddedMessageHdr *hdr = (UDPPaddedMessageHdr *)pkt;
	int nMsgLength = msg.ByteSize();
	if ( nMsgLength + sizeof(*hdr) > k_cbSteamNetworkingSocketsMaxUDPMsgLen )
	{
		AssertMsg3( false, "Msg type %d is %d bytes, larger than MTU of %d bytes", int( nMsgID ), int( nMsgLength + sizeof(*hdr) ), (int)sizeof(pkt) );
		return;
	}
	hdr->m_nMsgID = nMsgID;
	hdr->m_nMsgLength = LittleWord( uint16( nMsgLength ) );
	uint8 *pEnd = msg.SerializeWithCachedSizesToArray( pkt + sizeof(*hdr) );
	int cbPkt = pEnd - pkt;
	Assert( cbPkt == int( sizeof(*hdr) + nMsgLength ) );
	cbPkt = MAX( cbPkt, k_cbSteamNetworkingMinPaddedPacketSize );

	SendPacket( pkt, cbPkt );
}

void CSteamNetworkConnectionUDP::SendPacket( const void *pkt, int cbPkt )
{
	iovec temp;
	temp.iov_base = const_cast<void*>( pkt );
	temp.iov_len = cbPkt;
	SendPacketGather( 1, &temp, cbPkt );
}

void CSteamNetworkConnectionUDP::SendPacketGather( int nChunks, const iovec *pChunks, int cbSendTotal )
{
	// Safety
	if ( !m_pSocket )
	{
		AssertMsg( false, "Attemt to send packet, but socket has been closed!" );
		return;
	}

	// Update stats
	m_statsEndToEnd.TrackSentPacket( cbSendTotal );

	// Hand over to operating system
	m_pSocket->BSendRawPacketGather( nChunks, pChunks );
}

void CSteamNetworkConnectionUDP::ConnectionStateChanged( ESteamNetworkingConnectionState eOldState )
{
	CSteamNetworkConnectionBase::ConnectionStateChanged( eOldState );

	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_Dead:
			return;

		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			break;

		case k_ESteamNetworkingConnectionState_Linger:
			break;

		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_Connected:
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
			break;
	}
}

#define ReportBadPacketIPv4( pszMsgType, /* fmt */ ... ) \
	ReportBadPacketFrom( m_pSocket->GetRemoteHostAddr(), pszMsgType, __VA_ARGS__ )

void CSteamNetworkConnectionUDP::PacketReceived( const void *pvPkt, int cbPkt, const netadr_t &adrFrom, CSteamNetworkConnectionUDP *pSelf )
{
	const uint8 *pPkt = static_cast<const uint8 *>( pvPkt );

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	if ( cbPkt < 5 )
	{
		ReportBadPacket( "packet", "%d byte packet is too small", cbPkt );
		return;
	}

	// Data packet is the most common, check for it first.  Also, does stat tracking.
	if ( *pPkt & 0x80 )
	{
		pSelf->Received_Data( pPkt, cbPkt, usecNow );
		return;
	}

	// Track stats for other packet types.
	pSelf->m_statsEndToEnd.TrackRecvPacket( cbPkt, usecNow );

	if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeReply )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ChallengeReply, msg )
		pSelf->Received_ChallengeReply( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectOK )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectOK, msg );
		pSelf->Received_ConnectOK( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectionClosed )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ConnectionClosed, msg )
		pSelf->Received_ConnectionClosed( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_NoConnection )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_NoConnection, msg )
		pSelf->Received_NoConnection( msg, usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ChallengeRequest )
	{
		ParsePaddedPacket( pvPkt, cbPkt, CMsgSteamSockets_UDP_ChallengeRequest, msg )
		pSelf->Received_ChallengeOrConnectRequest( "ChallengeRequest", msg.connection_id(), usecNow );
	}
	else if ( *pPkt == k_ESteamNetworkingUDPMsg_ConnectRequest )
	{
		ParseProtobufBody( pPkt+1, cbPkt-1, CMsgSteamSockets_UDP_ConnectRequest, msg )
		pSelf->Received_ChallengeOrConnectRequest( "ConnectRequest", msg.client_connection_id(), usecNow );
	}
	else
	{
		ReportBadPacket( "packet", "Lead byte 0x%02x not a known message ID", *pPkt );
	}
}

std::string DescribeStatsContents( const CMsgSteamSockets_UDP_Stats &msg )
{
	std::string sWhat;
	if ( msg.flags() & msg.ACK_REQUEST_E2E )
		sWhat += " request_ack";
	if ( msg.flags() & msg.ACK_REQUEST_IMMEDIATE )
		sWhat += " request_ack_immediate";
	if ( msg.stats().has_lifetime() )
		sWhat += " stats.life";
	if ( msg.stats().has_instantaneous() )
		sWhat += " stats.rate";
	return sWhat;
}

void CSteamNetworkConnectionUDP::RecvStats( const CMsgSteamSockets_UDP_Stats &msgStatsIn, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// Connection quality stats?
	if ( msgStatsIn.has_stats() )
		m_statsEndToEnd.ProcessMessage( msgStatsIn.stats(), usecNow );

	// Spew appropriately
	SpewVerbose( "[%s] Recv %s stats:%s\n",
		GetDescription(),
		bInline ? "inline" : "standalone",
		DescribeStatsContents( msgStatsIn ).c_str()
	);

	// Check if we need to reply, either now or later
	if ( BStateIsConnectedForWirePurposes() )
	{

		// Check for queuing outgoing acks
		bool bImmediate = ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_IMMEDIATE ) != 0;
		if ( ( msgStatsIn.flags() & msgStatsIn.ACK_REQUEST_E2E ) || msgStatsIn.has_stats() )
		{
			QueueEndToEndAck( bImmediate, usecNow );
		}

		// Do we need to send an immediate reply?
		const char *pszReason = NeedToSendEndToEndStatsOrAcks( usecNow );
		if ( pszReason )
		{
			// Send a stats message
			SendStatsMsg( k_EStatsReplyRequest_NothingToSend, usecNow, pszReason );
		}
	}
}

void CSteamNetworkConnectionUDP::TrackSentStats( const CMsgSteamSockets_UDP_Stats &msgStatsOut, bool bInline, SteamNetworkingMicroseconds usecNow )
{

	// What effective flags will be received?
	bool bAllowDelayedReply = ( msgStatsOut.flags() & msgStatsOut.ACK_REQUEST_IMMEDIATE ) == 0;

	// Record that we sent stats and are waiting for peer to ack
	if ( msgStatsOut.has_stats() )
	{
		m_statsEndToEnd.TrackSentStats( msgStatsOut.stats(), usecNow, bAllowDelayedReply );
	}
	else if ( msgStatsOut.flags() & msgStatsOut.ACK_REQUEST_E2E )
	{
		m_statsEndToEnd.TrackSentMessageExpectingSeqNumAck( usecNow, bAllowDelayedReply );
	}

	// Spew appropriately
	SpewVerbose( "[%s] Sent %s stats:%s\n",
		GetDescription(),
		bInline ? "inline" : "standalone",
		DescribeStatsContents( msgStatsOut ).c_str()
	);
}

void CSteamNetworkConnectionUDP::Received_Data( const uint8 *pPkt, int cbPkt, SteamNetworkingMicroseconds usecNow )
{

	if ( cbPkt < sizeof(UDPDataMsgHdr) )
	{
		ReportBadPacketIPv4( "DataPacket", "Packet of size %d is too small.", cbPkt );
		return;
	}

	// Check cookie
	const UDPDataMsgHdr *hdr = (const UDPDataMsgHdr *)pPkt;
	if ( LittleDWord( hdr->m_unToConnectionID ) != m_unConnectionIDLocal )
	{

		// Wrong session.  It could be an old session, or it could be spoofed.
		ReportBadPacketIPv4( "DataPacket", "Incorrect connection ID" );
		if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		{
			SendNoConnection( LittleDWord( hdr->m_unToConnectionID ), 0 );
		}
		return;
	}
	uint16 nWirePktNumber = LittleWord( hdr->m_unSeqNum );

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Linger:
			// FIXME: What should we do here?  We are half-closed here, so this
			// data is definitely going to be ignored.  Do we need to communicate
			// that state to the remote host somehow?
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			// Ignore it.  We don't have the SteamID of whoever is on the other end yet,
			// their encryption keys, etc.  The most likely cause is that a server sent
			// a ConnectOK, which dropped.  So they think we're connected but we don't
			// have everything yet.
			return;

		case k_ESteamNetworkingConnectionState_Connected:

			// We'll process the chunk
			break;
	}

	const uint8 *pIn = pPkt + sizeof(*hdr);
	const uint8 *pPktEnd = pPkt + cbPkt;

	// Inline stats?
	static CMsgSteamSockets_UDP_Stats msgStats;
	CMsgSteamSockets_UDP_Stats *pMsgStatsIn = nullptr;
	uint32 cbStatsMsgIn = 0;
	if ( hdr->m_unMsgFlags & hdr->kFlag_ProtobufBlob )
	{
		//Msg_Verbose( "Received inline stats from %s", server.m_szName );

		pIn = DeserializeVarInt( pIn, pPktEnd, cbStatsMsgIn );
		if ( pIn == NULL )
		{
			ReportBadPacketIPv4( "DataPacket", "Failed to varint decode size of stats blob" );
			return;
		}
		if ( pIn + cbStatsMsgIn > pPktEnd )
		{
			ReportBadPacketIPv4( "DataPacket", "stats message size doesn't make sense.  Stats message size %d, packet size %d", cbStatsMsgIn, cbPkt );
			return;
		}

		if ( !msgStats.ParseFromArray( pIn, cbStatsMsgIn ) )
		{
			ReportBadPacketIPv4( "DataPacket", "protobuf failed to parse inline stats message" );
			return;
		}

		// Shove sequence number so we know what acks to pend, etc
		msgStats.set_seq_num( nWirePktNumber );
		pMsgStatsIn = &msgStats;

		// Advance pointer
		pIn += cbStatsMsgIn;
	}

	const void *pChunk = pIn;
	int cbChunk = pPktEnd - pIn;

	// Decrypt it, and check packet number
	uint8 arDecryptedChunk[ k_cbSteamNetworkingSocketsMaxPlaintextPayloadRecv ];
	uint32 cbDecrypted = sizeof(arDecryptedChunk);
	int64 nFullSequenceNumber = DecryptDataChunk( nWirePktNumber, cbPkt, pChunk, cbChunk, arDecryptedChunk, cbDecrypted, usecNow );
	if ( nFullSequenceNumber <= 0 )
		return;

	// Process plaintext
	if ( !ProcessPlainTextDataChunk( nFullSequenceNumber, arDecryptedChunk, cbDecrypted, 0, usecNow ) )
		return;

	// Process the stats, if any
	if ( pMsgStatsIn )
		RecvStats( *pMsgStatsIn, true, usecNow );
}

void CSteamNetworkConnectionUDP::Received_ChallengeReply( const CMsgSteamSockets_UDP_ChallengeReply &msg, SteamNetworkingMicroseconds usecNow )
{
	// We should only be getting this if we are the "client"
	if ( m_pParentListenSocket )
	{
		ReportBadPacketIPv4( "ChallengeReply", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Ignore if we're not trying to connect
	if ( GetState() != k_ESteamNetworkingConnectionState_Connecting )
		return;

	// Check session ID to make sure they aren't spoofing.
	if ( msg.connection_id() != m_unConnectionIDLocal )
	{
		ReportBadPacketIPv4( "ChallengeReply", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}
	if ( msg.protocol_version() < k_nMinRequiredProtocolVersion )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_Generic, "Peer is running old software and needs to be udpated" );
		return;
	}

	// Update ping, if they replied with the timestamp
	if ( msg.has_your_timestamp() )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - (SteamNetworkingMicroseconds)msg.your_timestamp();
		if ( usecElapsed < 0 || usecElapsed > 2*k_nMillion )
		{
			SpewWarning( "Ignoring weird timestamp %llu in ChallengeReply, current time is %llu.\n", (unsigned long long)msg.your_timestamp(), usecNow );
		}
		else
		{
			int nPing = (usecElapsed + 500 ) / 1000;
			m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Make sure we have the crypt info that we need
	if ( !m_msgSignedCertLocal.has_cert() || !m_msgSignedCryptLocal.has_info() )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Misc_InternalError, "Tried to connect request, but crypt not ready" );
		return;
	}

	// Remember protocol version.  They must send it again in the connect OK, but we have a valid value now,
	// so we might as well save it
	m_statsEndToEnd.m_nPeerProtocolVersion = msg.protocol_version();

	// Reply with the challenge data and our cert
	CMsgSteamSockets_UDP_ConnectRequest msgConnectRequest;
	msgConnectRequest.set_client_connection_id( m_unConnectionIDLocal );
	msgConnectRequest.set_challenge( msg.challenge() );
	msgConnectRequest.set_my_timestamp( usecNow );
	if ( m_statsEndToEnd.m_ping.m_nSmoothedPing >= 0 )
		msgConnectRequest.set_ping_est_ms( m_statsEndToEnd.m_ping.m_nSmoothedPing );
	*msgConnectRequest.mutable_cert() = m_msgSignedCertLocal;
	*msgConnectRequest.mutable_crypt() = m_msgSignedCryptLocal;

	// If the cert is generic, then we need to specify our identity
	if ( !m_bCertHasIdentity )
	{
		SteamNetworkingIdentityToProtobuf( m_identityLocal, msgConnectRequest, identity_string, legacy_identity_binary, legacy_client_steam_id );
	}
	else
	{
		// Identity is in the cert.  But for old peers, set legacy field, if we are a SteamID
		if ( m_identityLocal.GetSteamID64() )
			msgConnectRequest.set_legacy_client_steam_id( m_identityLocal.GetSteamID64() );
	}


	SendMsg( k_ESteamNetworkingUDPMsg_ConnectRequest, msgConnectRequest );

	// Reset timeout/retry for this reply.  But if it fails, we'll start
	// the whole handshake over again.  It keeps the code simpler, and the
	// challenge value has a relatively short expiry anyway.
	m_usecWhenSentConnectRequest = usecNow;
	EnsureMinThinkTime( usecNow + k_usecConnectRetryInterval );

	// They are supposed to reply with a timestamps, from which we can estimate the ping.
	// So this counts as a ping request
	m_statsEndToEnd.TrackSentPingRequest( usecNow, false );
}

void CSteamNetworkConnectionUDP::Received_ConnectOK( const CMsgSteamSockets_UDP_ConnectOK &msg, SteamNetworkingMicroseconds usecNow )
{
	SteamDatagramErrMsg errMsg;

	// We should only be getting this if we are the "client"
	if ( m_pParentListenSocket )
	{
		ReportBadPacketIPv4( "ConnectOK", "Shouldn't be receiving this unless on accepted connections, only connections initiated locally." );
		return;
	}

	// Check connection ID to make sure they aren't spoofing and it's the same connection we think it is
	if ( msg.client_connection_id() != m_unConnectionIDLocal )
	{
		ReportBadPacketIPv4( "ConnectOK", "Incorrect connection ID.  Message is stale or could be spoofed, ignoring." );
		return;
	}

	// Parse out identity from the cert
	SteamNetworkingIdentity identityRemote;
	bool bIdentityInCert = true;
	{
		// !SPEED! We are deserializing the cert here,
		// and then we are going to do it again below.
		// Should refactor to fix this.
		int r = SteamNetworkingIdentityFromSignedCert( identityRemote, msg.cert(), errMsg );
		if ( r < 0 )
		{
			ReportBadPacketIPv4( "ConnectRequest", "Bad identity in cert.  %s", errMsg );
			return;
		}
		if ( r == 0 )
		{
			// No identity in the cert.  Check if they put it directly in the connect message
			bIdentityInCert = false;
			r = SteamNetworkingIdentityFromProtobuf( identityRemote, msg, identity_string, legacy_identity_binary, legacy_server_steam_id, errMsg );
			if ( r < 0 )
			{
				ReportBadPacketIPv4( "ConnectRequest", "Bad identity.  %s", errMsg );
				return;
			}
			if ( r == 0 )
			{
				// If no identity was presented, it's the same as them saying they are "localhost"
				identityRemote.SetLocalHost();
			}
		}
	}
	Assert( !identityRemote.IsInvalid() );

	// Check if they are using an IP address as an identity (possibly the anonymous "localhost" identity)
	if ( identityRemote.m_eType == k_ESteamNetworkingIdentityType_IPAddress )
	{
		SteamNetworkingIPAddr addr;
		const netadr_t &adrFrom = m_pSocket->GetRemoteHostAddr();
		adrFrom.GetIPV6( addr.m_ipv6 );
		addr.m_port = adrFrom.GetPort();

		if ( identityRemote.IsLocalHost() )
		{
			if ( m_connectionConfig.m_IP_AllowWithoutAuth.Get() == 0 )
			{
				// Should we send an explicit rejection here?
				ReportBadPacketIPv4( "ConnectOK", "Unauthenticated connections not allowed." );
				return;
			}

			// Set their identity to their real address (including port)
			identityRemote.SetIPAddr( addr );
		}
		else
		{
			// FIXME - Should the address be required to match?
			// If we are behind NAT, it won't.
			//if ( memcmp( addr.m_ipv6, identityRemote.m_ip.m_ipv6, sizeof(addr.m_ipv6) ) != 0
			//	|| ( identityRemote.m_ip.m_port != 0 && identityRemote.m_ip.m_port != addr.m_port ) ) // Allow 0 port in the identity to mean "any port"
			//{
			//	ReportBadPacket( "ConnectRequest", "Identity in request is %s, but packet is coming from %s." );
			//	return;
			//}

			// It's not really clear what the use case is here for
			// requesting a specific IP address as your identity,
			// and not using localhost.  If they have a cert, assume it's
			// meaningful.  Remember: the cert could be unsigned!  That
			// is a separate issue which will be handled later, whether
			// we want to allow that.
			if ( !bIdentityInCert )
			{
				// Should we send an explicit rejection here?
				ReportBadPacket( "ConnectOK", "Cannot use specific IP address." );
				return;
			}
		}
	}

	// Make sure they are still who we think they are
	if ( !m_identityRemote.IsInvalid() && !( m_identityRemote == identityRemote ) )
	{
		ReportBadPacketIPv4( "ConnectOK", "server_steam_id doesn't match who we expect to be connecting to!" );
		return;
	}

	// Update ping, if they replied a timestamp
	if ( msg.has_your_timestamp() )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - (SteamNetworkingMicroseconds)msg.your_timestamp() - msg.delay_time_usec();
		if ( usecElapsed < 0 || usecElapsed > 2*k_nMillion )
		{
			SpewWarning( "Ignoring weird timestamp %llu in ConnectOK, current time is %llu, remote delay was %lld.\n", (unsigned long long)msg.your_timestamp(), usecNow, (long long)msg.delay_time_usec() );
		}
		else
		{
			int nPing = (usecElapsed + 500 ) / 1000;
			m_statsEndToEnd.m_ping.ReceivedPing( nPing, usecNow );
		}
	}

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
			// We already know we were able to establish the connection.
			// Just ignore this packet
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			break;
	}

	// Connection ID
	m_unConnectionIDRemote = msg.server_connection_id();
	if ( ( m_unConnectionIDRemote & 0xffff ) == 0 )
	{
		ConnectionState_ProblemDetectedLocally( k_ESteamNetConnectionEnd_Remote_BadCrypt, "Didn't send valid connection ID" );
		return;
	}

	m_identityRemote = identityRemote;

	// Check the certs, save keys, etc
	if ( !BRecvCryptoHandshake( msg.cert(), msg.crypt(), false ) )
	{
		Assert( GetState() == k_ESteamNetworkingConnectionState_ProblemDetectedLocally );
		ReportBadPacketIPv4( "ConnectOK", "Failed crypto init.  %s", m_szEndDebug );
		return;
	}

	// Generic connection code will take it from here.
	ConnectionState_Connected( usecNow );
}

void CSteamNetworkConnectionUDP::Received_ConnectionClosed( const CMsgSteamSockets_UDP_ConnectionClosed &msg, SteamNetworkingMicroseconds usecNow )
{
	// Give them a reply to let them know we heard from them.  If it's the right connection ID,
	// then they probably aren't spoofing and it's critical that we give them an ack!
	//
	// If the wrong connection ID, then it could be an old connection so we'd like to send a reply
	// to let them know that they can stop telling us the connection is closed.
	// However, it could just be random garbage, so we need to protect ourselves from abuse,
	// so limit how many of these we send.
	bool bConnectionIDMatch =
		msg.to_connection_id() == m_unConnectionIDLocal
		|| ( msg.to_connection_id() == 0 && msg.from_connection_id() && msg.from_connection_id() == m_unConnectionIDRemote ); // they might not know our ID yet, if they are a client aborting the connection really early.
	if ( bConnectionIDMatch || BCheckGlobalSpamReplyRateLimit( usecNow ) )
	{
		// Send a reply, echoing exactly what they sent to us
		CMsgSteamSockets_UDP_NoConnection msgReply;
		if ( msg.to_connection_id() )
			msgReply.set_from_connection_id( msg.to_connection_id() );
		if ( msg.from_connection_id() )
			msgReply.set_to_connection_id( msg.from_connection_id() );
		SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msgReply );
	}

	// If incorrect connection ID, then that's all we'll do, since this packet actually
	// has nothing to do with current connection at all.
	if ( !bConnectionIDMatch )
		return;

	// Generic connection code will take it from here.
	ConnectionState_ClosedByPeer( msg.reason_code(), msg.debug().c_str() );
}

void CSteamNetworkConnectionUDP::Received_NoConnection( const CMsgSteamSockets_UDP_NoConnection &msg, SteamNetworkingMicroseconds usecNow )
{
	// Make sure it's an ack of something we would have sent
	if ( msg.to_connection_id() != m_unConnectionIDLocal || msg.from_connection_id() != m_unConnectionIDRemote )
	{
		ReportBadPacketIPv4( "NoConnection", "Old/incorrect connection ID.  Message is for a stale connection, or is spoofed.  Ignoring." );
		return;
	}

	// Generic connection code will take it from here.
	ConnectionState_ClosedByPeer( 0, nullptr );
}

void CSteamNetworkConnectionUDP::Received_ChallengeOrConnectRequest( const char *pszDebugPacketType, uint32 unPacketConnectionID, SteamNetworkingMicroseconds usecNow )
{
	// If wrong connection ID, then check for sending a generic reply and bail
	if ( unPacketConnectionID != m_unConnectionIDRemote )
	{
		ReportBadPacketIPv4( pszDebugPacketType, "Incorrect connection ID, when we do have a connection for this address.  Could be spoofed, ignoring." );
		// Let's not send a reply in this case
		//if ( BCheckGlobalSpamReplyRateLimit( usecNow ) )
		//	SendNoConnection( unPacketConnectionID );
		return;
	}

	// Check state
	switch ( GetState() )
	{
		case k_ESteamNetworkingConnectionState_Dead:
		case k_ESteamNetworkingConnectionState_None:
		case k_ESteamNetworkingConnectionState_FindingRoute: // not used for raw UDP
		default:
			Assert( false );
			return;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_FinWait:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			SendConnectionClosedOrNoConnection();
			return;

		case k_ESteamNetworkingConnectionState_Connecting:
			// We're waiting on the application.  So we'll just have to ignore.
			break;

		case k_ESteamNetworkingConnectionState_Linger:
		case k_ESteamNetworkingConnectionState_Connected:
			if ( !m_pParentListenSocket )
			{
				// WAT?  We initiated this connection, so why are they requesting to connect?
				ReportBadPacketIPv4( pszDebugPacketType, "We are the 'client' who initiated the connection, so 'server' shouldn't be sending us this!" );
				return;
			}

			// This is totally legit and possible.  Our earlier reply might have dropped, and they are re-sending
			SendConnectOK( usecNow );
			return;
	}

}

void CSteamNetworkConnectionUDP::SendConnectionClosedOrNoConnection()
{
	if ( GetState() == k_ESteamNetworkingConnectionState_ClosedByPeer )
	{
		SendNoConnection( m_unConnectionIDLocal, m_unConnectionIDRemote );
	}
	else
	{
		CMsgSteamSockets_UDP_ConnectionClosed msg;
		msg.set_from_connection_id( m_unConnectionIDLocal );

		if ( m_unConnectionIDRemote )
			msg.set_to_connection_id( m_unConnectionIDRemote );

		msg.set_reason_code( m_eEndReason );
		if ( m_szEndDebug[0] )
			msg.set_debug( m_szEndDebug );
		SendPaddedMsg( k_ESteamNetworkingUDPMsg_ConnectionClosed, msg );
	}
}

void CSteamNetworkConnectionUDP::SendNoConnection( uint32 unFromConnectionID, uint32 unToConnectionID )
{
	CMsgSteamSockets_UDP_NoConnection msg;
	if ( unFromConnectionID == 0 && unToConnectionID == 0 )
	{
		AssertMsg( false, "Can't send NoConnection, we need at least one of from/to connection ID!" );
		return;
	}
	if ( unFromConnectionID )
		msg.set_from_connection_id( unFromConnectionID );
	if ( unToConnectionID )
		msg.set_to_connection_id( unToConnectionID );
	SendMsg( k_ESteamNetworkingUDPMsg_NoConnection, msg );
}

void CSteamNetworkConnectionUDP::SendConnectOK( SteamNetworkingMicroseconds usecNow )
{
	Assert( m_unConnectionIDLocal );
	Assert( m_unConnectionIDRemote );
	Assert( m_pParentListenSocket );

	Assert( m_msgSignedCertLocal.has_cert() );
	Assert( m_msgSignedCryptLocal.has_info() );

	CMsgSteamSockets_UDP_ConnectOK msg;
	msg.set_client_connection_id( m_unConnectionIDRemote );
	msg.set_server_connection_id( m_unConnectionIDLocal );
	*msg.mutable_cert() = m_msgSignedCertLocal;
	*msg.mutable_crypt() = m_msgSignedCryptLocal;

	// If the cert is generic, then we need to specify our identity
	if ( !m_bCertHasIdentity )
	{
		SteamNetworkingIdentityToProtobuf( m_identityLocal, msg, identity_string, legacy_identity_binary, legacy_server_steam_id );
	}
	else
	{
		// Identity is in the cert.  But for old peers, set legacy field, if we are a SteamID
		if ( m_identityLocal.GetSteamID64() )
			msg.set_legacy_server_steam_id( m_identityLocal.GetSteamID64() );
	}

	// Do we have a timestamp?
	if ( m_usecWhenReceivedHandshakeRemoteTimestamp )
	{
		SteamNetworkingMicroseconds usecElapsed = usecNow - m_usecWhenReceivedHandshakeRemoteTimestamp;
		Assert( usecElapsed >= 0 );
		if ( usecElapsed < 4*k_nMillion )
		{
			msg.set_your_timestamp( m_ulHandshakeRemoteTimestamp );
			msg.set_delay_time_usec( usecElapsed );
		}
		else
		{
			SpewWarning( "Discarding handshake timestamp that's %lldms old, not sending in ConnectOK\n", usecElapsed/1000 );
			m_usecWhenReceivedHandshakeRemoteTimestamp = 0;
		}
	}


	// Send it, with padding
	SendMsg( k_ESteamNetworkingUDPMsg_ConnectOK, msg );
}

EUnsignedCert CSteamNetworkConnectionUDP::AllowRemoteUnsignedCert()
{
	// NOTE: No special override for localhost.
	// Should we add a seperat3e convar for this?
	// For the CSteamNetworkConnectionlocalhostLoopback connection,
	// we know both ends are us.  but if they are just connecting to
	// 127.0.0.1, it's not clear that we should handle this any
	// differently from any other connection

	// Enabled by convar?
	int nAllow = m_connectionConfig.m_IP_AllowWithoutAuth.Get();
	if ( nAllow > 1 )
		return k_EUnsignedCert_Allow;
	if ( nAllow == 1 )
		return k_EUnsignedCert_AllowWarn;

	// Lock it down
	return k_EUnsignedCert_Disallow;
}

EUnsignedCert CSteamNetworkConnectionUDP::AllowLocalUnsignedCert()
{
	// Same logic actually applies for remote and local
	return AllowRemoteUnsignedCert();
}

/////////////////////////////////////////////////////////////////////////////
//
// Loopback connections
//
/////////////////////////////////////////////////////////////////////////////

CSteamNetworkConnectionlocalhostLoopback::CSteamNetworkConnectionlocalhostLoopback( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, const SteamNetworkingIdentity &identity )
: CSteamNetworkConnectionUDP( pSteamNetworkingSocketsInterface )
{
	m_identityLocal = identity;
}

EUnsignedCert CSteamNetworkConnectionlocalhostLoopback::AllowRemoteUnsignedCert()
{
	return k_EUnsignedCert_Allow;
}

EUnsignedCert CSteamNetworkConnectionlocalhostLoopback::AllowLocalUnsignedCert()
{
	return k_EUnsignedCert_Allow;
}

void CSteamNetworkConnectionlocalhostLoopback::PostConnectionStateChangedCallback( ESteamNetworkingConnectionState eOldAPIState, ESteamNetworkingConnectionState eNewAPIState )
{
	// Don't post any callbacks for the initial transitions.
	if ( eNewAPIState == k_ESteamNetworkingConnectionState_Connecting || eNewAPIState == k_ESteamNetworkingConnectionState_Connected )
		return;

	// But post callbacks for these guys
	CSteamNetworkConnectionUDP::PostConnectionStateChangedCallback( eOldAPIState, eNewAPIState );
}

bool CSteamNetworkConnectionlocalhostLoopback::APICreateSocketPair( CSteamNetworkingSockets *pSteamNetworkingSocketsInterface, CSteamNetworkConnectionlocalhostLoopback *pConn[2], const SteamNetworkingIdentity pIdentity[2] )
{
	SteamDatagramTransportLock::AssertHeldByCurrentThread();

	SteamDatagramErrMsg errMsg;

	pConn[1] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface, pIdentity[0] );
	pConn[0] = new CSteamNetworkConnectionlocalhostLoopback( pSteamNetworkingSocketsInterface, pIdentity[1] );
	if ( !pConn[0] || !pConn[1] )
	{
failed:
		delete pConn[0]; pConn[0] = nullptr;
		delete pConn[1]; pConn[1] = nullptr;
		return false;
	}

	IBoundUDPSocket *sock[2];
	if ( !CreateBoundSocketPair(
		CRecvPacketCallback( CSteamNetworkConnectionUDP::PacketReceived, (CSteamNetworkConnectionUDP*)pConn[0] ),
		CRecvPacketCallback( CSteamNetworkConnectionUDP::PacketReceived, (CSteamNetworkConnectionUDP*)pConn[1] ), sock, errMsg ) )
	{
		// Use assert here, because this really should only fail if we have some sort of bug
		AssertMsg1( false, "Failed to create UDP socekt pair.  %s", errMsg );
		goto failed;
	}

	SteamNetworkingMicroseconds usecNow = SteamNetworkingSockets_GetLocalTimestamp();

	// Initialize both connections
	for ( int i = 0 ; i < 2 ; ++i )
	{
		pConn[i]->m_pSocket = sock[i];
		if ( !pConn[i]->BInitConnection( usecNow, 0, nullptr, errMsg ) )
		{
			AssertMsg1( false, "CSteamNetworkConnectionlocalhostLoopback::BInitConnection failed.  %s", errMsg );
			goto failed;
		}
	}

	// Tie the connections to each other, and mark them as connected
	for ( int i = 0 ; i < 2 ; ++i )
	{
		CSteamNetworkConnectionlocalhostLoopback *p = pConn[i];
		CSteamNetworkConnectionlocalhostLoopback *q = pConn[1-i];
		p->m_identityRemote = q->m_identityLocal;
		p->m_unConnectionIDRemote = q->m_unConnectionIDLocal;
		p->m_statsEndToEnd.m_usecTimeLastRecv = usecNow; // Act like we just now received something
		if ( !p->BRecvCryptoHandshake( q->m_msgSignedCertLocal, q->m_msgSignedCryptLocal, i==0 ) )
		{
			AssertMsg( false, "BRecvCryptoHandshake failed creating localhost socket pair" );
			goto failed;
		}
		p->ConnectionState_Connected( usecNow );
	}

	return true;
}

} // namespace SteamNetworkingSocketsLib
