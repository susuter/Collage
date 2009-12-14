
/* Copyright (c) 2009, Cedric Stalder <cedric.stalder@gmail.com>
 *                     Stefan Eilemann <eile@equalizergraphics.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "log.h"
#include "rspConnection.h"
#include "connection.h"
#include "connectionDescription.h"

#include <eq/base/sleep.h>

//#define EQ_INSTRUMENT_RSP
#define SELF_INTERRUPT 42

#ifdef WIN32
#  define SELECT_TIMEOUT WAIT_TIMEOUT
#  define SELECT_ERROR   WAIT_FAILED
#else
#  define SELECT_TIMEOUT  0
#  define SELECT_ERROR   -1
#endif

#ifndef INFINITE
#  define INFINITE -1
#endif

namespace eq
{
namespace net
{

static const size_t _mtu = UDPConnection::getMTU();
static const size_t _ackFreq = UDPConnection::getPacketRate();
const size_t RSPConnection::_payloadSize =
    _mtu - sizeof( RSPConnection::DatagramData );
const size_t RSPConnection::_bufferSize = _payloadSize * _ackFreq;
static const size_t _nBuffers = 4;
const size_t RSPConnection::_maxNAck =
    _mtu - sizeof( RSPConnection::DatagramNack ) / sizeof( uint32_t );

namespace
{
#ifdef EQ_INSTRUMENT_RSP
base::a_int32_t nReadDataAccepted;
base::a_int32_t nReadData;
base::a_int32_t nBytesRead;
base::a_int32_t nBytesWritten;
base::a_int32_t nDatagrams;
base::a_int32_t nTotalDatagrams;
base::a_int32_t nAckRequests;
base::a_int32_t nTotalAckRequests;
base::a_int32_t nAcksSend;
base::a_int32_t nAcksSendTotal;
base::a_int32_t nAcksRead;
base::a_int32_t nAcksAccepted;
base::a_int32_t nNAcksSend;
base::a_int32_t nNAcksRead;
base::a_int32_t nNAcksResend;
base::a_int32_t nTimeOuts;
base::a_int32_t nTimeInWrite;
base::a_int32_t nTimeInWriteWaitAck;
base::a_int32_t nTimeInReadSync;
base::a_int32_t nTimeInReadData;
base::a_int32_t nTimeInHandleData;
#endif
}



RSPConnection::RSPConnection()
        : _countAcceptChildren( 0 )
        , _id( 0 )
        , _shiftedID( 0 )
        , _writerID( 0 )
        , _timeouts( 0 )
        , _ackReceived( false )
#ifdef WIN32
        , _hEvent( 0 )
#endif
        , _writing( false )
        , _numWriteAcks( 0 )
        , _thread ( 0 )
        , _connection( 0 )
        , _parent( 0 )
        , _lastSequenceIDAck( -1 )
        , _lengthDataSend( 0 )
        , _nDatagrams( 0 )
        , _sequenceIDWrite( 0 )
        , _readBufferIndex ( 0 )
        , _recvBufferIndex ( 0 )
        , _repeatData( false )
{
    _buildNewID();
    _description->type = CONNECTIONTYPE_RSP;
    _description->bandwidth = 102400;

    for( size_t i = 0; i < _nBuffers; ++i )
        _buffer.push_back( new DataReceive() );
    
    _recvBuffer = _buffer[ _recvBufferIndex ];
    EQLOG( net::LOG_RSP ) 
              << "Build new RSP Connection"  << std::endl
              << "rsp max buffer :" << _bufferSize << std::endl
              << "udp MTU :" << UDPConnection::getMTU() << std::endl;
}


void RSPConnection::DataReceive::reset() 
{
    sequenceID = 0;
    ackSend    = true;
    allRead    = true;
    posRead    = 0;

    got.resize( _ackFreq );
    data.resize( _bufferSize );
    memset( got.getData(), false, got.getSize( ));
}

void RSPConnection::close()
{
    if( _state == STATE_CLOSED )
        return;

    _state = STATE_CLOSED;

    if( _thread )
    {
        const DatagramNode exitNode ={ ID_EXIT, _id };
        _connection->write( &exitNode, sizeof( DatagramNode ) );
        _connectionSet.interrupt();
        _thread->join();
    }

    for( std::vector< RSPConnectionPtr >::iterator i = _children.begin();
        i != _children.end(); ++i )
    {
        RSPConnectionPtr connection = *i;
        connection = 0;
    }

    _parent = 0;

    if( _connection.isValid( ))
        _connection->close();

    _connection = 0;

    for( std::vector< DataReceive* >::iterator i = _buffer.begin(); 
         i < _buffer.end(); ++i )
    {
        DataReceive* buffer = *i;
        buffer->reset();
    }

    _recvBuffer = _buffer[0];
    _nackBuffer.clear();
    _fireStateChanged();
}

RSPConnection::~RSPConnection()
{
    close();

    _recvBuffer = 0;

    for( std::vector< DataReceive* >::iterator i = _buffer.begin(); 
         i < _buffer.end(); ++i )
    {
        DataReceive* buffer = *i;
        delete buffer;
    }

    _buffer.clear();
}

//----------------------------------------------------------------------
// Async IO handles
//----------------------------------------------------------------------
#ifdef WIN32
void RSPConnection::_initAIORead()
{
    _hEvent = CreateEvent( 0, TRUE, FALSE, 0 );
    EQASSERT( _hEvent );

    if( !_hEvent )
        EQERROR << "Can't create event for AIO notification: " 
                << base::sysError << std::endl;
}

void RSPConnection::_exitAIORead()
{
    if( _hEvent )
    {
        CloseHandle( _hEvent );
        _hEvent = 0;
    }
}
#else
void RSPConnection::_initAIORead(){ /* NOP */ }
void RSPConnection::_exitAIORead(){ /* NOP */ }
#endif


RSPConnection::ID RSPConnection::_buildNewID()
{
    _id = _rng.get<ID>();
    _shiftedID = static_cast< ID >( _id ) << ( sizeof( ID ) * 8 );
    return _id;
}

bool RSPConnection::listen()
{
    EQASSERT( _description->type == CONNECTIONTYPE_RSP );

    if( _state != STATE_CLOSED )
        return false;
    
    _state = STATE_CONNECTING;
    _fireStateChanged();

    // init an udp Connection
    _connection = new UDPConnection();
    ConnectionDescriptionPtr description = 
        new ConnectionDescription( *_description.get( ));
    description->type = CONNECTIONTYPE_UDP;
    _connection->setDescription( description );

    // connect UDP multicast
    if( !_connection->connect() )
    {
        EQWARN << "can't connect RSP transmission " << std::endl;
        return false;
    }

    _description = new ConnectionDescription( *description.get( ));
    _description->type = CONNECTIONTYPE_RSP;

    _connectionSet.addConnection( _connection.get( ));

    // init a thread for manage the communication protocol 
    _thread = new Thread( this );

#ifdef WIN32
    _initAIOAccept();
#else
    _numWriteAcks =  0;

    _selfPipeHEvent = new PipeConnection;
    if( !_selfPipeHEvent->connect( ))
    {
        EQERROR << "Could not create connection" << std::endl;
        return false;
    }

    _hEvent.events = POLLIN;
    _hEvent.fd = _selfPipeHEvent->getNotifier();
    _readFD = _hEvent.fd;
    _hEvent.revents = 0;
    _selfPipeHEvent->recvNB( &_selfCommand, sizeof( _selfCommand ));
#endif
    _numWriteAcks = 0;
    
    _readBuffer.resize( _connection->getMTU( ));

    // waits until RSP protocol establishes connection to the multicast network
    if( !_thread->start( ) )
    {
        close();
        return false;
    }
    _thread->start( );
    _state = STATE_LISTENING;
    _fireStateChanged();

    EQINFO << "Listening on " << _description->getHostname() << ":"
           << _description->port << " (" << _description->toString() << " @"
           << (void*)this << ")"
           << std::endl;
    return true;
}



ConnectionPtr RSPConnection::acceptSync()
{
    CHECK_THREAD( _recvThread );
    if( _state != STATE_LISTENING )
        return 0;

    EQASSERT ( _countAcceptChildren < static_cast< int >( _children.size( )));

    RSPConnectionPtr newConnection = _children[ _countAcceptChildren ];

    newConnection->_initAIORead();
    newConnection->_parent      = this;
    newConnection->_connection  = 0;
    newConnection->_state       = STATE_CONNECTED;
    newConnection->_description = _description;

#ifndef WIN32
    newConnection->_selfPipeHEvent = new PipeConnection;
    if( !newConnection->_selfPipeHEvent->connect( ))
    {
        EQERROR << "Could not create connection" << std::endl;
        return false;
    }
    
    newConnection->_hEvent.events = POLLIN;
    newConnection->_hEvent.fd = newConnection->_selfPipeHEvent->getNotifier();
    newConnection->_readFD = newConnection->_hEvent.fd;
    newConnection->_hEvent.revents = 0;

#endif

    ++_countAcceptChildren;
    _sendDatagramCountNode();
    
    EQINFO << "accepted connection " << (void*)newConnection.get()
           << std::endl;
    base::ScopedMutex mutexConn( _mutexConnection );
#ifdef WIN32
    if ( static_cast< int >( _children.size() ) <= _countAcceptChildren )
        ResetEvent( _hEvent );
    else 
        SetEvent( _hEvent );
#else
    _selfPipeHEvent->recvSync( 0, 0 );
    _selfPipeHEvent->recvNB( &_selfCommand, sizeof( _selfCommand ));
#endif

    ConnectionPtr connection = newConnection.get();
    return connection;
}

int64_t RSPConnection::readSync( void* buffer, const uint64_t bytes )
{
#ifdef EQ_INSTRUMENT_RSP
    base::Clock clock;
    clock.reset();
#endif
    const uint32_t size =   EQ_MIN( bytes, _bufferSize );
    DataReceive* receiver = _buffer[ _readBufferIndex ];
    
    receiver->ackSend.waitEQ( true );
    receiver->allRead.waitEQ( false );
    int64_t sizeRead = _readSync( receiver, buffer, size );
#ifdef EQ_INSTRUMENT_RSP
    nTimeInReadSync += clock.getTime64();
    nBytesRead += sizeRead;
#endif

    return sizeRead;
}

bool RSPConnection::_acceptID()
{
    _connection->readNB( _readBuffer.getData(), _mtu );

    // send a first datagram for announce me and discover other connection 
    const DatagramNode newnode ={ ID_HELLO, _id };
    _connection->write( &newnode, sizeof( DatagramNode ) );
    _timeouts = 0;
    while ( true )
    {
        switch ( _connectionSet.select( 100 ) )
        {
            case ConnectionSet::EVENT_TIMEOUT:
                ++_timeouts;
                if ( _timeouts < 10 )
                {
                   const DatagramNode ackNode ={ ID_HELLO, _id };
                   _connection->write( &ackNode, sizeof( DatagramNode ) );
                }
                else 
                {
                    const DatagramNode confirmNode ={ ID_CONFIRM, _id };
                    _connection->write( &confirmNode, sizeof( DatagramNode ) );
                    _addNewConnection( _id );
                    return true;
                }
                break;

            case ConnectionSet::EVENT_DATA:
                if( !_handleAcceptID() )
                {
                    EQERROR << " Error during Read UDP Connection" 
                            << std::endl;
                    return false;
                }

                _connection->readNB( _readBuffer.getData(), _mtu );
                break;

            case ConnectionSet::EVENT_INTERRUPT:
            default:
                break;
        }
    }
}

bool RSPConnection::_handleAcceptID()
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Error read on Connection UDP" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData() );
    const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                      ( _readBuffer.getData( ));
    switch ( *type )
    {
    case ID_HELLO:
        _checkNewID( node->connectionID );
        return true;

    case ID_DENY:
        // a connection refused my ID, ask for another ID
        if( node->connectionID == _id ) 
        {
            _timeouts = 0;
            const DatagramNode newnode = { ID_HELLO, _buildNewID() };
            _connection->write( &newnode, sizeof( DatagramNode ));
        }
        return true;

    case ID_EXIT:
        return _acceptRemoveConnection( node->connectionID );

    default: break;
    
    }

    return true;
}

bool RSPConnection::_initReadThread()
{
    // send a first datagram to announce me and discover other connections 
    _sendDatagramCountNode();
    _timeouts = 0;
    while ( true )
    {
        switch ( _connectionSet.select( 100 ) )
        {
            case ConnectionSet::EVENT_TIMEOUT:
                ++_timeouts;
                if( _timeouts < 10 )
                    _sendDatagramCountNode();
                else
                    return true;
                break;

            case ConnectionSet::EVENT_DATA:
                if ( !_handleInitData() )
                {
                    EQERROR << " Error during Read UDP Connection" 
                            << std::endl;
                    return false;
                }

                _connection->readNB( _readBuffer.getData(), _mtu );
                break;

            case ConnectionSet::EVENT_INTERRUPT:
            default:
                break;
        }
    }
}

bool RSPConnection::_handleInitData()
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Read error" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData() );
    const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                      ( _readBuffer.getData( ));
    switch ( *type )
    {
    case ID_HELLO:
        _timeouts = 0;
        _checkNewID( node->connectionID ) ;
        return true;

    case ID_CONFIRM:
        _timeouts = 0;
        return _addNewConnection( node->connectionID );

    case COUNTNODE:
    {
        const DatagramCountConnection* countConn = 
                reinterpret_cast< const DatagramCountConnection* >
                                                      ( _readBuffer.getData( ));
        
        // we know all connections
        if (( _children.size() == countConn->nbClient ) &&
            ( _children.size() > 1 )) 
        {
            _timeouts = 20;
            return true;
        }
        RSPConnectionPtr connection = 
                            _findConnectionWithWriterID( countConn->clientID );

        if ( !connection.isValid() )
        {
            _timeouts = 0;
            _addNewConnection( countConn->clientID );
        }
        break;
    }
    
    case ID_EXIT:
        return _acceptRemoveConnection( node->connectionID );

    default: break;
    
    }//END switch
    return true;

}

void* RSPConnection::Thread::run()
{
    _connection->_runReadThread();
    _connection = 0;
    return 0;
}

void RSPConnection::_runReadThread()
{
    EQINFO << "Started RSP read thread" << std::endl;
    while ( _state != STATE_CLOSED )
    {
#if 1
        const int timeOut = ( _writing && _repeatQueue.isEmpty( )) ? 10 : -1;
#else
        int timeOut = -1;
        if( _writing )
        {
            const int64_t sendRate = _connection->getSendRate();
            timeout = 102400 / sendRate;
            timeout = EQ_MIN
        }
#endif
        const ConnectionSet::Event event = _connectionSet.select( timeOut );
        switch( event )
        {
        case ConnectionSet::EVENT_TIMEOUT:
        {
#ifdef EQ_INSTRUMENT_RSP
            ++nTimeOuts;
#endif
            ++_timeouts;
            if( _timeouts >= 1000 )
            {
                EQERROR << "Error during send, too many timeouts" << std::endl;
                _connection = 0;
                return;
            }
            
            // repeat ack request
            _repeatQueue.push( RepeatRequest( RepeatRequest::ACKREQ ));
            break;
        }
        case ConnectionSet::EVENT_DATA:
        {
#ifdef EQ_INSTRUMENT_RSP
            base::Clock clock;
            clock.reset();
#endif            
            if ( !_handleData() )
               return;

            _connection->readNB( _readBuffer.getData(), _mtu );
#ifdef EQ_INSTRUMENT_RSP
            nTimeInHandleData += clock.getTime64();
#endif
            break;
        }
        case ConnectionSet::EVENT_INTERRUPT:
            break;
        default: 
            return;
        }
    }
}

int64_t RSPConnection::_readSync( DataReceive* receive, 
                                  void* buffer, 
                                  const uint64_t bytes )
{
    
    EQLOG( net::LOG_RSP ) << "start thread RSP " << std::endl;

    const uint64_t size = EQ_MIN( bytes, receive->data.getSize() - 
                                         receive->posRead );
    const uint8_t* data = receive->data.getData() + receive->posRead;
    memcpy( buffer, data, size );

    receive->posRead += size;
    
    // if all data in the buffer has been taken
    if( receive->posRead == receive->data.getSize( ))
    {
        EQLOG( net::LOG_RSP ) << "reset receiver" << std::endl;
        memset( receive->got.getData(), false, receive->got.getSize( ));
        
        _readBufferIndex = ( _readBufferIndex + 1 ) % _nBuffers;
        {
            base::ScopedMutex mutexEvent( _mutexEvent );
            if ( !( _buffer[ _readBufferIndex ]->ackSend.get() && 
                  !_buffer[ _readBufferIndex ]->allRead.get()) )
            {
#ifdef WIN32
                ResetEvent( _hEvent );
            }
            else
            {
                SetEvent( _hEvent );

#else
                _selfPipeHEvent->recvNB( &_selfCommand, sizeof( _selfCommand ));
                _selfPipeHEvent->recvSync( 0, 0);
#endif
            }
            receive->data.setSize( 0 );
            receive->allRead = true;
        }
    }

    return size;
}

bool RSPConnection::_handleData( )
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Error read on Connection UDP" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData( )); 
    switch ( *type )
    {
    case DATA:
        {

#ifdef EQ_INSTRUMENT_RSP
            base::Clock clock;
            clock.reset();
#endif
            bool resultRead = _handleDataDatagram( 
              reinterpret_cast< const DatagramData* >( _readBuffer.getData( )));

#ifdef EQ_INSTRUMENT_RSP
            nTimeInReadData += clock.getTime64();
#endif
            return resultRead;
        }

    case ACK:
        return _handleAck( reinterpret_cast< const DatagramAck* >( type )) ;
    
    case NACK:
        return _handleNack( reinterpret_cast< const DatagramNack* >
                                                     ( _readBuffer.getData() ));
    
    case ACKREQ: // The writer ask for a ack data
        return _handleAckRequest(
                reinterpret_cast< const DatagramAckRequest* >( type ));
    
    case ID_HELLO:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                     (  _readBuffer.getData() );
        _checkNewID( node->connectionID );
        return true;
    }

    case ID_CONFIRM:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                     (  _readBuffer.getData() );
        return _addNewConnection( node->connectionID );
    }

    case ID_EXIT:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                    (  _readBuffer.getData()  );
        return _acceptRemoveConnection( node->connectionID );
    }

    case COUNTNODE:
    {
        const DatagramCountConnection* countConn = 
                reinterpret_cast< const DatagramCountConnection* >
                                                      ( _readBuffer.getData() );
        
        // we know all connections
        if ( _children.size() == countConn->nbClient )
            return true;

        RSPConnectionPtr connection = 
                            _findConnectionWithWriterID( countConn->clientID );

        if ( !connection.isValid() )
            _addNewConnection( countConn->clientID );
        break;
    }
    default: break;
    }//END switch
    return true;
}

bool RSPConnection::_handleDataDatagram( const DatagramData* datagram )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nReadData;
#endif
    const uint32_t writerID = datagram->writeSeqID >> ( sizeof( ID ) * 8 );
    RSPConnectionPtr connection = _findConnectionWithWriterID( writerID );
    
    // it's an unknown connection or when we run netperf client before
    // server netperf
    // TO DO find a solution in this situation
    if ( !connection.isValid() )
    {
        EQUNREACHABLE;
        return false;
    }

    // if the buffer hasn't been found during previous read or last
    // ack data sequence.
    // find the data corresponding buffer 
    // why we haven't a receiver here:
    // 1: it's a reception data for another connection
    // 2: all buffer was not ready during last ack data
    const uint32_t sequenceID = datagram->writeSeqID  & 0xFFFF;
    if ( !connection->_recvBuffer )
    {
        EQLOG( net::LOG_RSP ) << "buffer not select, search a free buffer" 
                              << std::endl;

        connection->_recvBuffer = 
            connection->_findReceiverWithSequenceID( sequenceID );
        // if needed set the vector length
        if ( connection->_recvBuffer )
        {
            if ( connection->_buffer[ 
                          connection->_recvBufferIndex ]->allRead.get() )
                connection->_recvBuffer = 
                     connection->_buffer[ connection->_recvBufferIndex ];
        }
        else if 
          ( connection->_buffer[ connection->_recvBufferIndex ]->allRead.get() )
        {
            connection->_recvBuffer = connection->_buffer[ 
                    connection->_recvBufferIndex ];
        }
        else
        {
            // we do not have a free buffer, which means that the receiver is
            // slower then our read thread. This should not happen, because now
            // we'll drop the data and will send a full NAK packet upon the 
            // Ack request, causing retransmission even though we'll drop it
            // again
            //EQWARN << "Reader to slow, dropping data" << std::endl;
            return true;
        }
    }

    DataReceive* receive = connection->_recvBuffer;

    // if it's the first datagram 
    if( receive->ackSend.get() )
    {
        if ( sequenceID == receive->sequenceID )
            return true;

        // if it's a datagram repetition for an other connection, we have
        // to ignore it
        if ( ( connection->_lastSequenceIDAck == sequenceID ) &&
             connection->_lastSequenceIDAck != -1 )
            return true;

        EQLOG( net::LOG_RSP ) << "receive data from " << writerID 
                              << " sequenceID " << sequenceID << std::endl;
        receive->sequenceID = sequenceID;
        receive->posRead   = 0;
        receive->data.setSize( 0 );
        receive->ackSend   = false;
    }

    const uint64_t index = datagram->dataIDlength >> 16;

    // if it's a repetition and we have the data then we ignore it
    if( receive->got[ index ] )
        return true;

#ifdef EQ_INSTRUMENT_RSP
    ++nReadDataAccepted;
#endif
    
    const uint16_t length = datagram->dataIDlength & 0xFFFF ; 
    const uint8_t* data = reinterpret_cast< const uint8_t* >( ++datagram );
    receive->got[ index ] = true;
    const uint64_t pos = ( index ) * ( _mtu - sizeof( DatagramData ));
    
    receive->data.grow( index * _payloadSize + length );
    memcpy( receive->data.getData() + pos, data, length );

    // control if the previous datagrams has been received
    if( index <= 0 || receive->got[ index - 1 ] )
        return true;

    EQLOG( net::LOG_RSP ) << "send early nack" << std::endl;
    const uint16_t indexMax = index-1;
    uint16_t indexMin = indexMax;
    
    while( indexMin != 0 )
    {
        if ( !receive->got[ indexMin - 1 ] )
        {
            indexMin--;
            continue;
        }   
        
        break;
    }
    const uint32_t repeatID = indexMax | ( indexMin << 16 ) ; 
    
    _sendNackDatagram ( connection->_writerID, sequenceID, 1, &repeatID );
    return true;
}

bool RSPConnection::_handleAck( const DatagramAck* ack )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nAcksRead;
#endif
    EQLOG( net::LOG_RSP ) << "Receive Ack from " << ack->writerID << std::endl
                          << " for sequence ID " << ack->sequenceID << std::endl
                          << " current sequence ID " << _sequenceIDWrite 
                          << std::endl;
    // ignore sequenceID which be different that my write sequence
    //  Reason : - repeat late ack
    //           - an ack for an other writer
    if ( !_isCurrentSequenceWrite( ack->sequenceID, ack->writerID ) )
    {
        EQLOG( net::LOG_RSP ) << "ignore Ack, it's not for me" << std::endl;
        return true;
    }
    // find connection destination and if we have not receive an ack from,
    // we update the ack data.
    RSPConnectionPtr connection = _findConnectionWithWriterID( ack->readerID );

    if ( !connection.isValid() )
    {
        EQUNREACHABLE;
        return false;
    }

    // if I have received an ack previously from the reader
    if ( connection->_ackReceived )
        return true;
#ifdef EQ_INSTRUMENT_RSP
    ++nAcksAccepted;
#endif
    connection->_ackReceived = true;
    ++_numWriteAcks;

    // reset timeout counter
    _timeouts = 0;

    if ( _numWriteAcks != _children.size() )
        return true;

    EQLOG( net::LOG_RSP ) << "unlock write function " << _sequenceIDWrite 
                          << std::endl;

    // unblock write function if all acks have been received
    _repeatQueue.push( RepeatRequest( RepeatRequest::DONE ));

    return true;
}

bool RSPConnection::_handleNack( const DatagramNack* nack )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nNAcksRead;
#endif

    RSPConnectionPtr connection = _findConnectionWithWriterID( nack->readerID );

    EQLOG( net::LOG_RSP ) << "receive Nack from reader "
                          << connection->_writerID << " for sequence "
                          << nack->sequenceID << std::endl;

    if ( connection->_ackReceived )
    {
        EQLOG( net::LOG_RSP ) << "ignore Nack, we have receive an ack before" 
                              << std::endl;
        return true;
    }

    if ( !_isCurrentSequenceWrite( nack->sequenceID, nack->writerID ) )
    {
        EQLOG( net::LOG_RSP ) << "ignore Nack, it's not for me" << std::endl;
        return true;
    }
    
    if ( !connection )
    {
        EQUNREACHABLE;
        return false;
        // it's an unknown connection 
        // TO DO add this connection?
    }

    _timeouts = 0;

    EQLOG( net::LOG_RSP ) << "Repeat data" << std::endl;

    const uint16_t count = nack->count;
    ++nack;

    _addRepeat( reinterpret_cast< const uint32_t* >( nack ), count );
    return true;
}

void RSPConnection::_addRepeat( const uint32_t* repeatIDs, uint32_t size )
{
    for( size_t j = 0; j < size; ++j )
    {
        RepeatRequest repeat;
        repeat.start = ( repeatIDs[j] & 0xFFFF0000) >> 16;
        repeat.end   = ( repeatIDs[j] & 0xFFFF );
        _repeatQueue.push( repeat );

        EQASSERT( repeat.end <= _nDatagrams );
        EQASSERT( repeat.start <= repeat.end);
    }
}

bool RSPConnection::_handleAckRequest( const DatagramAckRequest* ackRequest )
{
    base::ScopedMutex mutex( _mutexHandleAckRequ );
    EQLOG( net::LOG_RSP ) << "receive an AckRequest from " 
                          << ackRequest->writerID << std::endl;
    RSPConnectionPtr connection = 
                            _findConnectionWithWriterID( ackRequest->writerID );

    if ( !connection.isValid() )
    {
        EQUNREACHABLE;
        return false;
    }
    // find the corresponding buffer
    DataReceive* receive = connection->_findReceiverWithSequenceID( 
                                               ackRequest->sequenceID );
    
    // Why no receiver found ?
    // 1 : all datagram data has not been received ( timeout )
    // 2 : all receivers are full and not ready to receive data
    //    We ask for resend of all datagrams
    if ( !receive )
    {
        EQLOG( net::LOG_RSP ) 
            << "receiver not found, ask for repeat all datagram " 
            << std::endl;
        const uint32_t repeatID = ackRequest->lastDatagramID; 
        _sendNackDatagram ( connection->_writerID, ackRequest->sequenceID,
                           1, &repeatID );
        return true;
    }
    
    EQLOG( net::LOG_RSP ) << "receiver found " << std::endl;

    // Repeat ack
    if ( receive->ackSend.get() )
    {
        EQLOG( net::LOG_RSP ) << "Repeat Ack for sequenceID : " 
                              << ackRequest->sequenceID << std::endl;
        _sendAck( ackRequest->writerID, ackRequest->sequenceID );
        return true;
    }
    
    // find all lost datagrams
    EQASSERT( ackRequest->lastDatagramID < receive->got.getSize( ));
    eq::base::Buffer< uint32_t > bufferRepeatID;

    for( size_t i = 0; i <= ackRequest->lastDatagramID; i++)
    {
        // size max datagram = mtu
        if( _maxNAck <= bufferRepeatID.getSize() )
            break;

        if( receive->got[i] )
            continue;
        
        EQLOG( net::LOG_RSP ) << "receiver Nack start " << i << std::endl;
        
        const uint32_t start = i << 16;
        uint32_t end = ackRequest->lastDatagramID;
        
        // OPT: Send all NACK packets at once
        for( ; i < receive->got.getSize(); i++ )
        {
            if( !receive->got[i] )
                continue;
            end = i-1;
            break;
        }
        EQLOG( net::LOG_RSP ) << "receiver Nack end " << end << std::endl;
        const uint32_t repeatID = end | start ; 
        bufferRepeatID.append( repeatID );
    }
    
    
    // send negative ack
    if( bufferRepeatID.getSize() > 0 )
    {
        EQLOG( net::LOG_RSP ) << "receiver send Nack to writerID  " 
                              << connection->_writerID << std::endl
                              << "sequenceID " << ackRequest->sequenceID
                              << std::endl;
        _sendNackDatagram ( connection->_writerID,
                            ackRequest->sequenceID,
                            bufferRepeatID.getSize(),
                            bufferRepeatID.getData() );
        return true;
    }
    
    // no repeat needed, we send an ack and we prepare the next buffer 
    // receive.
    connection->_recvBuffer = 0;
    
    // Found a free buffer for the next receive
    EQLOG( net::LOG_RSP ) << "receiver send Ack to writerID  " 
                          << connection->_writerID << std::endl
                          << "sequenceID " << ackRequest->sequenceID
                          << std::endl;

    connection->_recvBufferIndex = 
                    ( connection->_recvBufferIndex + 1 ) % _nBuffers;

    if ( connection->_buffer[ connection->_recvBufferIndex ]->allRead.get() )
    {
        EQLOG( net::LOG_RSP ) << "set next buffer  " << std::endl;
        connection->_recvBuffer = 
                 connection->_buffer[ connection->_recvBufferIndex ];
    }
    else
    {
        EQLOG( net::LOG_RSP ) << "can't set next buffer  " << std::endl;
    }

    _sendAck( connection->_writerID, receive->sequenceID );

#ifdef EQ_INSTRUMENT_RSP
    ++nAcksSend;
#endif

    connection->_lastSequenceIDAck = receive->sequenceID;
    {
        base::ScopedMutex mutexEvent( _mutexEvent );
        EQLOG( net::LOG_RSP ) << "data ready set Event for sequence" 
                              << receive->sequenceID << std::endl;
#ifdef WIN32
        SetEvent( connection->_hEvent );
#else
        const char c = SELF_INTERRUPT;
        connection->_selfPipeHEvent->send( &c, 1, true );
#endif
        // the receiver is ready and can be read by ReadSync
        receive->ackSend = true;
        receive->allRead = false;
    }

    return true;
}

void RSPConnection::_checkNewID( ID id )
{
    if ( id == _id )
    {
        DatagramNode nodeSend = { ID_DENY, _id };
        _connection->write( &nodeSend, sizeof( DatagramNode ) );
        return;
    }
    
    // look if the new ID exist in another connection
    RSPConnectionPtr child = _findConnectionWithWriterID( id );
    if ( child.isValid() )
    {
        DatagramNode nodeSend = { ID_DENY, id };
        _connection->write( &nodeSend, sizeof( DatagramNode ) );
    }
    
    return;
}

RSPConnection::DataReceive* RSPConnection::_findReceiverWithSequenceID( 
                                         const IDSequenceType sequenceID ) const
{
    // find the correspondig buffer
    for( std::vector< DataReceive* >::const_iterator k = _buffer.begin(); 
          k != _buffer.end(); ++k )
    {
        if( (*k)->sequenceID == sequenceID )
            return *k;
    }
    return 0;
}

RSPConnection::RSPConnectionPtr RSPConnection::_findConnectionWithWriterID( 
                                    const ID writerID )
{
    for( size_t i = 0 ; i < _children.size(); ++i )
    {
        if ( _children[i]->_writerID == writerID )
            return _children[i];
    }
    return 0;
}


bool RSPConnection::_addNewConnection( ID id )
{
    if ( _findConnectionWithWriterID( id ).isValid() )
        return true;

    RSPConnection* connection  = new RSPConnection();
    connection->_connection    = 0;
    connection->_writerID      = id;

    // protect the event and child size which can be use at the same time 
    // in acceptSync
    {
        base::ScopedMutex mutexConn( _mutexConnection );
        _children.push_back( connection );
        EQWARN << "New connection " << id << std::endl;

#ifdef WIN32
        SetEvent( _hEvent );
#else
        const char c = SELF_INTERRUPT;
        _selfPipeHEvent->send( &c, 1, true );
#endif
    }
    _sendDatagramCountNode();
    return true;
}

bool RSPConnection::_acceptRemoveConnection( const ID id )
{
    EQWARN << "remove Connection " << id << std::endl;
    if ( id != _id )
    {
        _sendDatagramCountNode();
        return true;
    }

    for( std::vector< RSPConnectionPtr >::iterator i = _children.begin(); 
          i != _children.end(); ++i )
    {
        RSPConnectionPtr child = *i;
        if ( child->_writerID == id )
        {
            child->close();
            _children.erase( i );
            break;
        }
    }

    _sendDatagramCountNode();
    return true;
}
bool RSPConnection::_isCurrentSequenceWrite( uint16_t sequenceID, 
                                             uint16_t writer )
{
    return  !(( sequenceID != _sequenceIDWrite ) || 
              ( writer != _id ));
}

int64_t RSPConnection::write( const void* buffer, const uint64_t bytes )
{
    if( _state != STATE_CONNECTED && _state != STATE_LISTENING )
        return -1;
    
    if ( _parent.isValid() )
        return _parent->write( buffer, bytes );

    const uint32_t size = EQ_MIN( bytes, _bufferSize );

#ifdef EQ_INSTRUMENT_RSP
    base::Clock clock;
    clock.reset();
    nBytesWritten += size;
#endif
    _timeouts = 0;
    _numWriteAcks = 0;

    ++_sequenceIDWrite;
    _lengthDataSend = size;
    _nDatagrams = size  / _payloadSize;
    
    // compute number of datagram
    if ( _nDatagrams * _payloadSize != size )
        _nDatagrams++;

    uint32_t writSeqID = _shiftedID | _sequenceIDWrite;

    EQLOG( net::LOG_RSP ) << "write sequence ID : " 
                          << _sequenceIDWrite << std::endl
                          << "number datagram " << _nDatagrams << std::endl;

    // init all ack received flags
    for( std::vector< RSPConnectionPtr >::iterator i = _children.begin();
        i != _children.end(); ++i )
    {
        (*i)->_ackReceived = false;
    }

    // send each datagram
    const uint8_t* data = reinterpret_cast< const uint8_t* >( buffer );
    for( size_t i = 0; i < _nDatagrams; ++i )
        _sendDatagram( data, writSeqID, i );
#ifdef EQ_INSTRUMENT_RSP
    nDatagrams += _nDatagrams;
#endif

    EQLOG( net::LOG_RSP ) << "write send Ack Request for " 
                          << _sequenceIDWrite << std::endl;

    _writing = true;
    _connectionSet.interrupt();

#ifdef EQ_INSTRUMENT_RSP
    ++nAckRequests;
    base::Clock clockAck;
    clockAck.reset();
#endif

    _sendAckRequest();
    _adaptSendRate( _handleRepeat( data ));

#ifdef EQ_INSTRUMENT_RSP
    nTimeInWriteWaitAck  += clockAck.getTime64();
    nTimeInWrite += clock.getTime64();

    if( bytes <= _bufferSize )
        EQWARN << *this << std::endl;
#endif

    EQLOG( net::LOG_RSP ) << "release write sequence ID " 
                          << _sequenceIDWrite << std::endl;
    return size;
}

int64_t RSPConnection::_handleRepeat( const uint8_t* data )
{
    const uint32_t writeSeqID = _shiftedID | _sequenceIDWrite;
    int64_t nRepeats = 0;

    while( true )
    {
        std::vector< RepeatRequest > requests;

        while( requests.empty( ))
        {
            const RepeatRequest& request = _repeatQueue.pop();
            switch( request.type )
            {
                case RepeatRequest::DONE:
                    _writing = false;
                    return nRepeats;

                case RepeatRequest::ACKREQ:
                    _sendAckRequest();
                    _connectionSet.interrupt();
                    break;

                case RepeatRequest::NACK:
                    requests.push_back( request );
                    break;

                default:
                    EQUNIMPLEMENTED;
            }
        }

        // merge nack requests
        while( !_repeatQueue.isEmpty( ))
        {
            const RepeatRequest& candidate = _repeatQueue.pop();
            switch( candidate.type )
            {
                case RepeatRequest::DONE:
                    _writing = false;
                    return nRepeats;

                case RepeatRequest::ACKREQ:
                    break; // ignore, will send one below anyway

                case RepeatRequest::NACK:
                    {
                        bool merged = false;
                        for( std::vector< RepeatRequest >::iterator i = 
                                requests.begin();
                             i != requests.end() && !merged; ++i )
                        {
                            RepeatRequest& old = *i;
                            if( old.start <= candidate.end &&
                                old.end >= candidate.start )
                            {
                                old.start = EQ_MIN( old.start, candidate.start);
                                old.end = EQ_MAX( old.end, candidate.end );
                                merged = true;
                            }
                        }

                        if( !merged )
                            requests.push_back( candidate );
                    }
            }
        }

        // calculate errors and adapt send rate
        uint64_t errors = 0;
        for( std::vector< RepeatRequest >::iterator i = requests.begin();
             i != requests.end(); ++i )
        {
            RepeatRequest& repeat = *i; 
            errors += repeat.end - repeat.start + 1;
            ++nRepeats;
        }
        _adaptSendRate( errors );

        // send merged requests
        for( std::vector< RepeatRequest >::iterator i = requests.begin();
             i != requests.end(); ++i )
        {
#ifdef EQ_INSTRUMENT_RSP
            ++nNAcksResend;
#endif
            RepeatRequest& repeat = *i;        
            EQASSERT( repeat.start <= repeat.end );

            for( size_t j = repeat.start; j <= repeat.end; ++j )
                _sendDatagram( data, writeSeqID, j );

        }

        // re-request ack
        if ( _repeatQueue.isEmpty() )
        {
            _sendAckRequest( );
            _connectionSet.interrupt();
        }
    }
}

void RSPConnection::_adaptSendRate( const uint64_t errors )
{
    const float error = ( 100.0 / 
                  static_cast< float >( _nDatagrams )*
                              static_cast< float >( errors ));

    if ( error <= 1.0f )
        _connection->adaptSendRate( 10 );
    else if ( error <= 2.0f )
        _connection->adaptSendRate( 1 );
    else if ( error <= 3.0f )
        _connection->adaptSendRate( -1 );
    else if ( error <= 5.0f )
        _connection->adaptSendRate( -5 ); 
    else if ( error <= 20.0f )
        _connection->adaptSendRate( -10 );
    else
        _connection->adaptSendRate( -20 );
}

void RSPConnection::_sendDatagramCountNode()
{
    if ( !_findConnectionWithWriterID( _id ) )
        return;

    const DatagramCountConnection countNode = 
                       { COUNTNODE, _id, _children.size() };
    _connection->write( &countNode, sizeof( DatagramCountConnection ) );
}

void RSPConnection::_sendAck( const ID writerID,
                              const IDSequenceType sequenceID)
{
#ifdef EQ_INSTRUMENT_RSP
    ++nAcksSendTotal;
#endif
    const DatagramAck ack = { ACK, _id, writerID, sequenceID };
    if ( _id == writerID )
        _handleAck( &ack );
    else
        _connection->write( &ack, sizeof( ack ) );
}

void RSPConnection::_sendNackDatagram ( const ID  toWriterID,
                                        const IDSequenceType  sequenceID,
                                        const uint8_t   count,
                                        const uint32_t* repeatID   )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nNAcksSend;
#endif
    /* optimization : we use the direct access to the reader. */
    if ( toWriterID == _id )
    {
         _addRepeat( repeatID, count );
         return ;
    }

    const size_t size = count * sizeof( uint32_t ) + sizeof( DatagramNack );    
    EQASSERT( size <= _mtu );

    _nackBuffer.reserve( size );

    // set the header
    DatagramNack* header = 
        reinterpret_cast< DatagramNack* >( _nackBuffer.getData( ));

    header->type = NACK;
    header->readerID = _id;
    header->writerID = toWriterID;
    header->sequenceID = sequenceID;
    header->count = count;

    ++header;

    memcpy( header, repeatID, size - sizeof( DatagramNack ));
    _connection->write( _nackBuffer.getData(), size );
}

void RSPConnection::_sendDatagram( const uint8_t* data,
                                   const uint32_t writeSeqID, 
                                   const uint16_t idDatagram )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nTotalDatagrams;
#endif
    const uint32_t posInData = _payloadSize * idDatagram;
    uint16_t lengthData;
    
    if ( _lengthDataSend - posInData >= _payloadSize )
        lengthData = _payloadSize;
    else
        lengthData = _lengthDataSend - posInData;
    
    const uint8_t* ptr = data + posInData;
    _sendBuffer.resize( lengthData + sizeof( DatagramData ) );
    
    uint32_t dataIDlength = ( idDatagram << 16 ) | lengthData;

    // set the header
    DatagramData* header = reinterpret_cast< DatagramData* >
                                                ( _sendBuffer.getData() );
    header->type = DATA;
    header->writeSeqID = writeSeqID;
    header->dataIDlength = dataIDlength;

    memcpy( ++header, ptr, lengthData );

    // send Data
    _handleDataDatagram( 
           reinterpret_cast< DatagramData* >( _sendBuffer.getData( )));
    _connection->waitWritable( _sendBuffer.getSize( ));
    _connection->write ( _sendBuffer.getData(), _sendBuffer.getSize() );
}

void RSPConnection::_sendAckRequest()
{

#ifdef EQ_INSTRUMENT_RSP
    ++nTotalAckRequests;
#endif
    const DatagramAckRequest ackRequest = { ACKREQ, _id, _nDatagrams -1, 
                                            _sequenceIDWrite };
    _handleAckRequest( &ackRequest );
    _connection->write( &ackRequest, sizeof( DatagramAckRequest ) );
}

std::ostream& operator << ( std::ostream& os,
                            const RSPConnection& connection )
{
    os << base::disableFlush << base::disableHeader << "RSPConnection id "
       << connection.getID() << " send rate " << connection.getSendRate();

#ifdef EQ_INSTRUMENT_RSP
    os << ": read " << nBytesRead << " bytes, wrote " 
       << nBytesWritten << " bytes using " << nDatagrams << " dgrams "
       << nTotalDatagrams - nDatagrams << " repeated, "
       << nTimeOuts << " write timeouts, "
       << std::endl
       << nAckRequests << " ack requests " 
       << nTotalAckRequests - nAckRequests << " repeated, "
       << nAcksAccepted << "/" << nAcksRead << " acks read, "
       << nNAcksResend << "/" << nNAcksRead << " nacks answered, "
       << std::endl
       << nAcksSend << " acks " << nAcksSendTotal - nAcksSend << " repeated, "
       << nNAcksSend << " negative acks "

       << std::endl
       << " time in write " << nTimeInWrite 
       << " ack wait time  "  << nTimeInWriteWaitAck
       << " nTimeInReadSync " << nTimeInReadSync
       << " nTimeInReadData " << nTimeInReadData
       << " nTimeInHandleData " << nTimeInHandleData;

    nReadDataAccepted = 0;
    nReadData = 0;
    nBytesRead = 0;
    nBytesWritten = 0;
    nDatagrams = 0;
    nTotalDatagrams = 0;
    nAckRequests = 0;
    nTotalAckRequests = 0;
    nAcksSend = 0;
    nAcksSendTotal = 0;
    nAcksRead = 0;
    nAcksAccepted = 0;
    nNAcksSend = 0;
    nNAcksRead = 0;
    nNAcksResend = 0;
    nTimeOuts = 0;
    nTimeInWrite = 0;
    nTimeInWriteWaitAck = 0;
    nTimeInReadSync = 0;
    nTimeInReadData = 0;
    nTimeInHandleData = 0;
#endif
    os << base::enableHeader << base::enableFlush;

    return os;
}

}
}
