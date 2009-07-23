// Revision: 1692

#include "yield/ipc.h"
using namespace YIELD;


// client.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#pragma warning( pop )
#define ETIMEDOUT WSAETIMEDOUT
#endif
template <class RequestType, class ResponseType>
Client<RequestType, ResponseType>::Client( const URI& absolute_uri, auto_AIOQueue aio_queue, uint32_t flags, auto_Log log, const Time& operation_timeout, auto_SocketAddress peername, auto_SSLContext ssl_context )
  : aio_queue( aio_queue ), flags( flags ), log( log ), operation_timeout( operation_timeout ), peername( peername ), ssl_context( ssl_context )
{
  this->absolute_uri = new URI( absolute_uri );
  operation_timer_queue = new TimerQueue;
}
template <class RequestType, class ResponseType>
Client<RequestType, ResponseType>::~Client()
{
  Object::decRef( *this->absolute_uri );
  for ( typename std::vector<Socket*>::iterator idle_socket_i = idle_sockets.begin(); idle_socket_i != idle_sockets.end(); idle_socket_i++ )
    Object::decRef( **idle_socket_i );
}
template <class RequestType, class ResponseType>
void Client<RequestType, ResponseType>::handleEvent( Event& ev )
{
  switch ( ev.get_tag() )
  {
    case YIELD_OBJECT_TAG( RequestType ):
    {
      RequestType& request = static_cast<RequestType&>( ev );
      if ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_OPERATIONS ) == this->CLIENT_FLAG_TRACE_OPERATIONS && log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client sending " << request.get_type_name() << "/" << reinterpret_cast<uint64_t>( &request ) << " to " << this->absolute_uri->get_host() << ":" << this->absolute_uri->get_port() << ".";
      if ( !idle_sockets.empty() )
      {
        Socket* socket_ = idle_sockets.back();
        idle_sockets.pop_back();
        if ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_OPERATIONS ) == this->CLIENT_FLAG_TRACE_OPERATIONS && log != NULL )
          log->getStream( Log::LOG_INFO ) << "yield::Client: writing " << request.get_type_name() << "/" << reinterpret_cast<uint64_t>( &request ) << " to " << this->absolute_uri->get_host() << ":" << this->absolute_uri->get_port() << ".";
        AIOWriteControlBlock* aio_write_control_block = new AIOWriteControlBlock( request.serialize(), ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_OPERATIONS ) == this->CLIENT_FLAG_TRACE_OPERATIONS && log != NULL ) ? log : NULL, request, operation_timeout, operation_timer_queue );
        operation_timer_queue->addTimer( new OperationTimer( aio_write_control_block->incRef(), operation_timeout ) );
        socket_->aio_write( aio_write_control_block );
      }
      else
      {
        auto_Socket socket_;
#ifdef YIELD_HAVE_OPENSSL
        if ( absolute_uri->get_scheme()[absolute_uri->get_scheme().size()-1] == 's' &&
             ssl_context != NULL )
          socket_ = SSLSocket::create( ssl_context ).release();
        else
#endif
        if ( absolute_uri->get_scheme()[absolute_uri->get_scheme().size()-1] == 'u' )
          socket_ = UDPSocket::create().release();
        else
          socket_ = TCPSocket::create().release();
        socket_->associate( aio_queue );
        if ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_IO ) == this->CLIENT_FLAG_TRACE_IO &&
             log != NULL && log->get_level() >= Log::LOG_INFO &&
             static_cast<int>( *socket_ ) != -1 )
          socket_ = new TracingSocket( socket_, log );
        if ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_OPERATIONS ) == this->CLIENT_FLAG_TRACE_OPERATIONS && log != NULL )
          log->getStream( Log::LOG_INFO ) << "yield::Client: connecting to " << this->absolute_uri->get_host() << ":" << this->absolute_uri->get_port() << ".";
        AIOConnectControlBlock* aio_connect_control_block = new AIOConnectControlBlock( peername, this->absolute_uri->incRef(), ( ( this->get_flags() & this->CLIENT_FLAG_TRACE_OPERATIONS ) == this->CLIENT_FLAG_TRACE_OPERATIONS && log != NULL ) ? log : NULL, request, operation_timeout, operation_timer_queue );
        operation_timer_queue->addTimer( new OperationTimer( aio_connect_control_block->incRef(), operation_timeout ) );
        socket_->aio_connect( aio_connect_control_block );
      }
    }
    break;
    default:
    {
      Object::decRef( ev );
    }
    break;
  }
}
template <class RequestType, class ResponseType>
class Client<RequestType, ResponseType>::AIOConnectControlBlock : public Socket::AIOConnectControlBlock
{
public:
  AIOConnectControlBlock( auto_SocketAddress peername, auto_URI absolute_uri, auto_Log log, auto_Object<RequestType> request, const Time& timeout, auto_TimerQueue timer_queue )
    : Socket::AIOConnectControlBlock( peername ),
      absolute_uri( absolute_uri ),
      log( log ),
      request( request ),
      timeout( timeout ),
      timer_queue( timer_queue )
  { }
  // AIOControlBlock
  void onCompletion( size_t )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: successfully connected to " << this->absolute_uri->get_host() << ":" << this->absolute_uri->get_port() << ".";
      AIOWriteControlBlock* aio_write_control_block = new AIOWriteControlBlock( request->serialize(), log, request, timeout, timer_queue );
      timer_queue->addTimer( new OperationTimer( aio_write_control_block->incRef(), timeout ) );
      get_socket()->aio_write( aio_write_control_block );
    }
  }
  void onError( uint32_t error_code )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: connect() to " << this->absolute_uri->get_host() << ":" << this->absolute_uri->get_port() << " failed, errno=" << error_code << ", strerror=" << Exception::strerror( error_code ) << ".";
      request->respond( *( new ExceptionResponse( error_code ) ) );
    }
  }
private:
  auto_URI absolute_uri;
  auto_Log log;
  auto_Object<RequestType> request;
  Mutex request_lock;
  Time timeout;
  auto_TimerQueue timer_queue;
};
template <class RequestType, class ResponseType>
class Client<RequestType, ResponseType>::AIOReadControlBlock : public Socket::AIOReadControlBlock
{
public:
  AIOReadControlBlock( auto_Buffer buffer, auto_Log log, auto_Object<RequestType> request, auto_Object<ResponseType> response, const Time& timeout, auto_TimerQueue timer_queue )
    : Socket::AIOReadControlBlock( buffer ),
      log( log ),
      request( request ), response( response ),
      timeout( timeout ),
      timer_queue( timer_queue )
  { }
  // AIOControlBlock
  void onCompletion( size_t bytes_transferred )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: read " << bytes_transferred << " bytes for " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << ".";
      Socket::AIOReadControlBlock::onCompletion( bytes_transferred );
      ssize_t deserialize_ret = response->deserialize( get_buffer() );
      if ( deserialize_ret == 0 )
      {
        if ( log != NULL )
          log->getStream( Log::LOG_INFO ) << "yield::Client: successfully deserialized " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << ", responding to " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << ".";
        request->respond( *response.release() );
      }
      else if ( deserialize_ret > 0 )
      {
        if ( log != NULL )
          log->getStream( Log::LOG_INFO ) << "yield::Client: partially deserialized " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << ", reading again.";
        AIOReadControlBlock* aio_read_control_block = new AIOReadControlBlock( new HeapBuffer( 1024 ), log, request, response, timeout, timer_queue );
        timer_queue->addTimer( new OperationTimer( aio_read_control_block->incRef(), timeout ) );
        get_socket()->aio_read( aio_read_control_block );
      }
      else
      {
        if ( log != NULL )
          log->getStream( Log::LOG_INFO ) << "yield::Client: error deserializing " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << ", responding to " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << " with ExceptionResponse.";
        request->respond( *( new ExceptionResponse ) );
      }
    }
  }
  void onError( uint32_t error_code )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: error reading " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << ", responding to " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << " with ExceptionResponse.";
      request->respond( *( new ExceptionResponse( error_code ) ) );
    }
  }
private:
  auto_Log log;
  auto_Object<RequestType> request;
  Mutex request_lock;
  auto_Object<ResponseType> response;
  Time timeout;
  auto_TimerQueue timer_queue;
};
template <class RequestType, class ResponseType>
class Client<RequestType, ResponseType>::AIOWriteControlBlock : public Socket::AIOWriteControlBlock
{
public:
  AIOWriteControlBlock( auto_Buffer buffer, auto_Log log, auto_Object<RequestType> request, const Time& timeout, auto_TimerQueue timer_queue )
    : Socket::AIOWriteControlBlock( buffer ),
      log( log ),
      request( request ),
      timeout( timeout ),
      timer_queue( timer_queue )
  { }
  // Object
  YIELD_OBJECT_PROTOTYPES( AIOWriteControlBlock, 0 );
  // AIOControlBlock
  void onCompletion( size_t bytes_transferred )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: wrote " << bytes_transferred << " bytes for " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << ".";
      auto_Object<ResponseType> response( static_cast<ResponseType*>( request->createResponse().release() ) );
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: created " << response->get_type_name() << "/" << reinterpret_cast<uint64_t>( response.get() ) << " to " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << ".";
      AIOReadControlBlock* aio_read_control_block = new AIOReadControlBlock( new HeapBuffer( 1024 ), log, request, response, timeout, timer_queue );
      timer_queue->addTimer( new OperationTimer( aio_read_control_block->incRef(), timeout ) );
      get_socket()->aio_read( aio_read_control_block );
    }
  }
  void onError( uint32_t error_code )
  {
    if ( request_lock.try_acquire() )
    {
      if ( log != NULL )
        log->getStream( Log::LOG_INFO ) << "yield::Client: error writing " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << ", responding to " << request->get_type_name() << "/" << reinterpret_cast<uint64_t>( request.get() ) << " with ExceptionResponse.";
      request->respond( *( new ExceptionResponse( error_code ) ) );
    }
  }
private:
  auto_Log log;
  auto_Object<RequestType> request;
  Mutex request_lock;
  Time timeout;
  auto_TimerQueue timer_queue;
};
template <class RequestType, class ResponseType>
class Client<RequestType, ResponseType>::OperationTimer : public TimerQueue::Timer
{
public:
  OperationTimer( auto_AIOControlBlock aio_control_block, const Time& operation_timeout )
    : TimerQueue::Timer( operation_timeout ),
      aio_control_block( aio_control_block )
  { }
  bool fire( const Time& )
  {
    aio_control_block->onError( ETIMEDOUT );
    return true;
  }
private:
  auto_AIOControlBlock( aio_control_block );
};
template class Client<HTTPRequest, HTTPResponse>;
template class Client<ONCRPCRequest, ONCRPCResponse>;


// http_benchmark_driver.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
TimerQueue HTTPBenchmarkDriver::statistics_timer_queue;
class HTTPBenchmarkDriver::StatisticsTimer : public TimerQueue::Timer
{
public:
  StatisticsTimer( auto_Object<HTTPBenchmarkDriver> http_benchmark_driver )
    : Timer( 5 * NS_IN_S, 5 * NS_IN_S ), http_benchmark_driver( http_benchmark_driver )
  { }
  // Timer
  bool fire( const Time& elapsed_time )
  {
    http_benchmark_driver->calculateStatistics( elapsed_time );
    return true;
  }
private:
  auto_Object<HTTPBenchmarkDriver> http_benchmark_driver;
};
HTTPBenchmarkDriver::HTTPBenchmarkDriver( auto_EventTarget http_request_target, uint8_t in_flight_http_request_count, const std::vector<URI*>& wlog_uris )
  : http_request_target( http_request_target ), in_flight_http_request_count( in_flight_http_request_count ), wlog_uris( wlog_uris )
{
  requests_sent_in_period = responses_received_in_period = 0;
  statistics_timer_queue.addTimer( new StatisticsTimer( incRef() ) );
}
HTTPBenchmarkDriver::~HTTPBenchmarkDriver()
{
  for ( std::vector<URI*>::iterator wlog_uri_i = wlog_uris.begin(); wlog_uri_i != wlog_uris.end(); wlog_uri_i++ )
    delete *wlog_uri_i;
}
auto_Object<HTTPBenchmarkDriver> HTTPBenchmarkDriver::create( auto_EventTarget http_request_target, uint8_t in_flight_http_request_count, const Path& wlog_file_path )
{
  auto_MemoryMappedFile wlog = MemoryMappedFile::open( wlog_file_path );
  if ( wlog != NULL )
  {
    char *wlog_p = static_cast<char*>( *wlog ), *wlog_end = wlog_p + wlog->get_size();
    std::vector<URI*> wlog_uris;
    while ( wlog_p < wlog_end )
    {
      char* uri_str = wlog_p;
      size_t uri_str_len = strnlen( uri_str, UINT16_MAX );
      auto_URI uri( URI::parse( uri_str, uri_str_len ) );
      if ( uri != NULL )
        wlog_uris.push_back( uri.release() );
      wlog_p += uri_str_len + 1;
    }
    std::reverse( wlog_uris.begin(), wlog_uris.end() ); // So we can pop them in the right order
    return new HTTPBenchmarkDriver( http_request_target, in_flight_http_request_count, wlog_uris );
  }
  else
    return NULL;
}
void HTTPBenchmarkDriver::calculateStatistics( const Time& elapsed_time )
{
  statistics_lock.acquire();
  double request_rate = static_cast<double>( requests_sent_in_period ) / elapsed_time.as_unix_time_s();
  request_rates.push_back( request_rate );
  requests_sent_in_period = 0;
  double response_rate = static_cast<double>( responses_received_in_period ) / elapsed_time.as_unix_time_s();
  response_rates.push_back( response_rate );
  responses_received_in_period = 0;
  statistics_lock.release();
}
void HTTPBenchmarkDriver::get_request_rates( std::vector<double>& out_request_rates )
{
  statistics_lock.acquire();
  out_request_rates.insert( out_request_rates.end(), request_rates.begin(), request_rates.end() );
  statistics_lock.release();
}
void HTTPBenchmarkDriver::get_response_rates( std::vector<double>& out_response_rates )
{
  statistics_lock.acquire();
  out_response_rates.insert( out_response_rates.end(), response_rates.begin(), response_rates.end() );
  statistics_lock.release();
}
void HTTPBenchmarkDriver::handleEvent( Event& ev )
{
  switch ( ev.get_tag() )
  {
    case YIELD_OBJECT_TAG( Stage::StartupEvent ):
    {
      my_stage = static_cast<Stage::StartupEvent&>( ev ).get_stage();
      Object::decRef( ev );
      uint8_t max_in_flight_http_request_count = in_flight_http_request_count;
      in_flight_http_request_count = 0;
      for ( uint8_t http_request_i = 0; http_request_i < max_in_flight_http_request_count; http_request_i++ )
        sendHTTPRequest();
    }
    break;
    case YIELD_OBJECT_TAG( HTTPResponse ):
    {
      in_flight_http_request_count--;
      responses_received_in_period++;
      Object::decRef( ev );
      sendHTTPRequest();
    }
    break;
    default: handleUnknownEvent( ev );
  }
}
void HTTPBenchmarkDriver::wait()
{
  wait_signal.acquire();
}
void HTTPBenchmarkDriver::sendHTTPRequest()
{
  if ( !wlog_uris.empty() )
  {
    URI* wlog_uri = wlog_uris.back();
    wlog_uris.pop_back();
    HTTPRequest* http_req = new HTTPRequest( "GET", wlog_uri->get_resource().c_str() );
    http_req->set_response_target( my_stage->incRef() );
    in_flight_http_request_count++;
    requests_sent_in_period++;
    http_request_target->send( *http_req );
  }
  else if ( in_flight_http_request_count == 0 )
    wait_signal.release();
}


// http_client.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
auto_HTTPClient HTTPClient::create( const URI& absolute_uri,
                                    auto_AIOQueue aio_queue,
                                    uint32_t flags,
                                    auto_Log log,
                                    const Time& operation_timeout,
                                    auto_SSLContext ssl_context )
{
  URI checked_absolute_uri( absolute_uri );
  if ( checked_absolute_uri.get_port() == 0 )
    checked_absolute_uri.set_port( 80 );
  auto_SocketAddress peername = SocketAddress::create( absolute_uri );
  if ( peername != NULL )
  {
    if ( aio_queue == NULL )
    {
      aio_queue = AIOQueue::create();
      if ( aio_queue == NULL )
        return NULL;
    }
#ifdef YIELD_HAVE_OPENSSL
    if ( absolute_uri.get_scheme() == "https" && ssl_context == NULL )
      ssl_context = SSLContext::create( SSLv23_client_method() );
#endif
    return new HTTPClient( absolute_uri, aio_queue, flags, log, operation_timeout, peername, ssl_context );
  }
  return NULL;
}
auto_HTTPResponse HTTPClient::GET( const URI& absolute_uri, auto_Log log )
{
  return sendHTTPRequest( "GET", absolute_uri, NULL, log );
}
auto_HTTPResponse HTTPClient::PUT( const URI& absolute_uri, auto_Buffer body, auto_Log log )
{
  return sendHTTPRequest( "PUT", absolute_uri, body, log );
}
auto_HTTPResponse HTTPClient::PUT( const URI& absolute_uri, const Path& body_file_path, auto_Log log )
{
  auto_File file = File::open( body_file_path );
  size_t file_size = static_cast<size_t>( file->getattr()->get_size() );
  auto_Object<HeapBuffer> body = new HeapBuffer( file_size );
  file->read( *body, file_size );
  return sendHTTPRequest( "PUT", absolute_uri, body.release(), log );
}
auto_HTTPResponse HTTPClient::sendHTTPRequest( const char* method, const URI& absolute_uri, auto_Buffer body, auto_Log log )
{
  auto_HTTPClient http_client = HTTPClient::create( absolute_uri, NULL, 0, log );
  auto_HTTPRequest http_request = new HTTPRequest( method, absolute_uri, body );
  http_request->set_header( "User-Agent", "Flog 0.99" );
  auto_EventQueue http_response_queue( new EventQueue );
  http_request->set_response_target( http_response_queue->incRef() );
  http_client->send( http_request->incRef() );
  return http_response_queue->dequeue_typed<HTTPResponse>();
}


// http_message.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
HTTPMessage::HTTPMessage( uint8_t reserve_iovecs_count )
  : RFC822Headers( reserve_iovecs_count )
{
  http_version = 1;
}
HTTPMessage::HTTPMessage( uint8_t reserve_iovecs_count, auto_Buffer body )
  : RFC822Headers( reserve_iovecs_count ), body( body )
{
  http_version = 1;
}
ssize_t HTTPMessage::deserialize( auto_Buffer buffer )
{
  switch ( deserialize_state )
  {
    case DESERIALIZING_HEADERS:
    {
      ssize_t RFC822Headers_deserialize_ret = RFC822Headers::deserialize( buffer );
      if ( RFC822Headers_deserialize_ret == 0 )
      {
        if ( strcmp( get_header( "Transfer-Encoding" ), "chunked" ) == 0 )
          return 0;
        else
        {
          const char* content_length_header_value = get_header( "Content-Length", NULL ); // Most browsers
          if ( content_length_header_value == NULL )
            content_length_header_value = get_header( "Content-length" ); // httperf
          size_t content_length = atoi( content_length_header_value );
          if ( content_length == 0 )
          {
            deserialize_state = DESERIALIZE_DONE;
            return 0;
          }
          else
          {
            deserialize_state = DESERIALIZING_BODY;
            if ( strcmp( get_header( "Expect" ), "100-continue" ) == 0 )
              return 0;
            // else fall through
          }
        }
      }
      else
        return RFC822Headers_deserialize_ret;
    }
    case DESERIALIZING_BODY:
    {
      if ( body == NULL )
        body = buffer;
      else
        DebugBreak(); // Chain buffers
      deserialize_state = DESERIALIZE_DONE;
    }
    case DESERIALIZE_DONE: return 0;
    default: DebugBreak(); return -1;
  }
}
auto_Buffer HTTPMessage::serialize()
{
  // Finalize headers
  if ( body != NULL )
  {
    if ( get_header( "Content-Length", NULL ) == NULL )
    {
      char content_length_str[32];
#ifdef _WIN32
      sprintf_s( content_length_str, 32, "%u", body->size() );
#else
      snprintf( content_length_str, 32, "%zu", body->size() );
#endif
      set_header( "Content-Length", content_length_str );
    }
    set_next_iovec( "\r\n", 2 );
    set_next_iovec( static_cast<const char*>( static_cast<void*>( *body ) ), body->size() );
  }
  else
    set_next_iovec( "\r\n", 2 );
  return RFC822Headers::serialize();
}


// http_request.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
HTTPRequest::HTTPRequest()
  : HTTPMessage( 0 )
{
  method[0] = 0;
  uri = new char[2];
  uri[0] = 0;
  uri_len = 2;
  http_version = 1;
  deserialize_state = DESERIALIZING_METHOD;
}
HTTPRequest::HTTPRequest( const char* method, const char* relative_uri, const char* host, auto_Buffer body )
  : HTTPMessage( 4, body )
{
  init( method, relative_uri, host, body );
}
HTTPRequest::HTTPRequest( const char* method, const URI& absolute_uri, auto_Buffer body )
  : HTTPMessage( 4, body )
{
  init( method, absolute_uri.get_resource().c_str(), absolute_uri.get_host().c_str(), body );
}
void HTTPRequest::init( const char* method, const char* relative_uri, const char* host, auto_Buffer body )
{
#ifdef _WIN32
  strncpy_s( this->method, 16, method, 16 );
#else
  strncpy( this->method, method, 16 );
#endif
  uri_len = strnlen( relative_uri, UINT16_MAX );
  this->uri = new char[uri_len + 1];
  memcpy_s( this->uri, uri_len + 1, relative_uri, uri_len + 1 );
  http_version = 1;
  set_header( "Host", const_cast<char*>( host ) );
  deserialize_state = DESERIALIZE_DONE;
}
HTTPRequest::~HTTPRequest()
{
  delete [] uri;
}
ssize_t HTTPRequest::deserialize( auto_Buffer buffer )
{
  switch ( deserialize_state )
  {
    case DESERIALIZING_METHOD:
    {
      char* method_p = method + strnlen( method, 16 );
      for ( ;; )
      {
        if ( buffer->get( method_p, 1 ) == 1 )
        {
          if ( *method_p != ' ' )
            method_p++;
          else
          {
            *method_p = 0;
            deserialize_state = DESERIALIZING_URI;
            break;
          }
        }
        else
        {
          *method_p = 0;
          return 1;
        }
      }
      // Fall through
    }
    case DESERIALIZING_URI:
    {
      char* uri_p = uri + strnlen( uri, UINT16_MAX );
      for ( ;; )
      {
        if ( buffer->get( uri_p, 1 ) == 1 )
        {
          if ( *uri_p == ' ' )
          {
            *uri_p = 0;
            uri_len = uri_p - uri;
            deserialize_state = DESERIALIZING_HTTP_VERSION;
            break;
          }
          else
          {
            uri_p++;
            if ( static_cast<size_t>( uri_p - uri ) == uri_len )
            {
              size_t new_uri_len = uri_len * 2;
              char* new_uri = new char[new_uri_len];
              memcpy_s( new_uri, new_uri_len, uri, uri_len );
              delete [] uri;
              uri = new_uri;
              uri_p = uri + uri_len;
              uri_len = new_uri_len;
            }
          }
        }
        else
        {
          *uri_p = 0;
          return 1;
        }
      }
      // Fall through
    }
    case DESERIALIZING_HTTP_VERSION:
    {
      for ( ;; )
      {
        uint8_t test_http_version;
        if ( buffer->get( &test_http_version, 1 ) == 1 )
        {
          if ( test_http_version != '\r' )
          {
            http_version = test_http_version;
            continue;
          }
          else
          {
            http_version = http_version == '1' ? 1 : 0;
            deserialize_state = DESERIALIZING_HEADERS;
            break;
          }
        }
        else
          return 1;
      }
    }
    // Fall through
    default: return HTTPMessage::deserialize( buffer );
  }
}
void HTTPRequest::respond( uint16_t status_code )
{
  respond( *( new HTTPResponse( status_code ) ) );
}
void HTTPRequest::respond( uint16_t status_code, auto_Buffer body )
{
  respond( *( new HTTPResponse( status_code, body ) ) );
}
auto_Buffer HTTPRequest::serialize()
{
  RFC822Headers::set_iovec( 0, method, strnlen( method, 16 ) );
  RFC822Headers::set_iovec( 1, " ", 1 );
  RFC822Headers::set_iovec( 2, uri, uri_len );
  RFC822Headers::set_iovec( 3, " HTTP/1.1\r\n", 11 );
  return HTTPMessage::serialize();
}


// http_response.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
HTTPResponse::HTTPResponse()
  : HTTPMessage( 0 )
{
  memset( status_code_str, 0, sizeof( status_code_str ) );
  deserialize_state = DESERIALIZING_HTTP_VERSION;
}
HTTPResponse::HTTPResponse( uint16_t status_code )
  : HTTPMessage( 1 ), status_code( status_code )
{
  http_version = 1;
  deserialize_state = DESERIALIZE_DONE;
}
HTTPResponse::HTTPResponse( uint16_t status_code, auto_Buffer body )
  : HTTPMessage( 1, body ), status_code( status_code )
{
  deserialize_state = DESERIALIZE_DONE;
}
ssize_t HTTPResponse::deserialize( auto_Buffer buffer )
{
  switch ( deserialize_state )
  {
    case DESERIALIZING_HTTP_VERSION:
    {
      for ( ;; )
      {
        uint8_t test_http_version;
        if ( buffer->get( &test_http_version, 1 ) == 1 )
        {
          if ( test_http_version != ' ' )
          {
            http_version = test_http_version;
            continue;
          }
          else
          {
            http_version = http_version == '1' ? 1 : 0;
            deserialize_state = DESERIALIZING_STATUS_CODE;
            break;
          }
        }
        else
          return 1;
      }
    }
    // Fall through
    case DESERIALIZING_STATUS_CODE:
    {
      char* status_code_str_p = status_code_str + strnlen( status_code_str, 3 );
      for ( ;; )
      {
        if ( buffer->get( status_code_str_p, 1 ) == 1 )
        {
          if ( *status_code_str_p != ' ' )
          {
            status_code_str_p++;
            if ( static_cast<uint8_t>( status_code_str_p - status_code_str ) == 4 )
            {
              deserialize_state = DESERIALIZE_DONE;
              return -1;
            }
          }
          else
          {
            *status_code_str_p = 0;
            status_code = static_cast<uint16_t>( atoi( status_code_str ) );
            if ( status_code == 0 )
              status_code = 500;
            deserialize_state = DESERIALIZING_REASON;
            break;
          }
        }
        else
          return 1;
      }
    }
    // Fall through
    case DESERIALIZING_REASON:
    {
      char c;
      for ( ;; )
      {
        if ( buffer->get( &c, 1 ) == 1 )
        {
          if ( c == '\r' )
          {
            deserialize_state = DESERIALIZING_HEADERS;
            break;
          }
        }
        else
          return 1;
      }
    }
    // Fall through
    default: return HTTPMessage::deserialize( buffer );
  }
}
auto_Buffer HTTPResponse::serialize()
{
  const char* status_line;
  size_t status_line_len;
  switch ( status_code )
  {
    case 100: status_line = "HTTP/1.1 100 Continue\r\n"; status_line_len = 23; break;
    case 200: status_line = "HTTP/1.1 200 OK\r\n"; status_line_len = 17; break;
    case 201: status_line = "HTTP/1.1 201 Created\r\n"; status_line_len = 22; break;
    case 202: status_line = "HTTP/1.1 202 Accepted\r\n"; status_line_len = 23; break;
    case 203: status_line = "HTTP/1.1 203 Non-Authoritative Information\r\n"; status_line_len = 44; break;
    case 204: status_line = "HTTP/1.1 204 No Content\r\n"; status_line_len = 25; break;
    case 205: status_line = "HTTP/1.1 205 Reset Content\r\n"; status_line_len = 28; break;
    case 206: status_line = "HTTP/1.1 206 Partial Content\r\n"; status_line_len = 30; break;
    case 207: status_line = "HTTP/1.1 207 Multi-Status\r\n"; status_line_len = 27; break;
    case 300: status_line = "HTTP/1.1 300 Multiple Choices\r\n"; status_line_len = 31; break;
    case 301: status_line = "HTTP/1.1 301 Moved Permanently\r\n"; status_line_len = 32; break;
    case 302: status_line = "HTTP/1.1 302 Found\r\n"; status_line_len = 20; break;
    case 303: status_line = "HTTP/1.1 303 See Other\r\n"; status_line_len = 24; break;
    case 304: status_line = "HTTP/1.1 304 Not Modified\r\n"; status_line_len = 27; break;
    case 305: status_line = "HTTP/1.1 305 Use Proxy\r\n"; status_line_len = 24; break;
    case 307: status_line = "HTTP/1.1 307 Temporary Redirect\r\n"; status_line_len = 33; break;
    case 400: status_line = "HTTP/1.1 400 Bad Request\r\n"; status_line_len = 26; break;
    case 401: status_line = "HTTP/1.1 401 Unauthorized\r\n"; status_line_len = 27; break;
    case 403: status_line = "HTTP/1.1 403 Forbidden\r\n"; status_line_len = 24; break;
    case 404: status_line = "HTTP/1.1 404 Not Found\r\n"; status_line_len = 24; break;
    case 405: status_line = "HTTP/1.1 405 Method Not Allowed\r\n"; status_line_len = 33; break;
    case 406: status_line = "HTTP/1.1 406 Not Acceptable\r\n"; status_line_len = 29; break;
    case 407: status_line = "HTTP/1.1 407 Proxy Authentication Required\r\n"; status_line_len = 44; break;
    case 408: status_line = "HTTP/1.1 408 Request Timeout\r\n"; status_line_len = 30; break;
    case 409: status_line = "HTTP/1.1 409 Conflict\r\n"; status_line_len = 23; break;
    case 410: status_line = "HTTP/1.1 410 Gone\r\n"; status_line_len = 19; break;
    case 411: status_line = "HTTP/1.1 411 Length Required\r\n"; status_line_len = 30; break;
    case 412: status_line = "HTTP/1.1 412 Precondition Failed\r\n"; status_line_len = 34; break;
    case 413: status_line = "HTTP/1.1 413 Request Entity Too Large\r\n"; status_line_len = 39; break;
    case 414: status_line = "HTTP/1.1 414 Request-URI Too Long\r\n"; status_line_len = 35; break;
    case 415: status_line = "HTTP/1.1 415 Unsupported Media Type\r\n"; status_line_len = 37; break;
    case 416: status_line = "HTTP/1.1 416 Request Range Not Satisfiable\r\n"; status_line_len = 44; break;
    case 417: status_line = "HTTP/1.1 417 Expectation Failed\r\n"; status_line_len = 33; break;
    case 422: status_line = "HTTP/1.1 422 Unprocessable Entitiy\r\n"; status_line_len = 36; break;
    case 423: status_line = "HTTP/1.1 423 Locked\r\n"; status_line_len = 21; break;
    case 424: status_line = "HTTP/1.1 424 Failed Dependency\r\n"; status_line_len = 32; break;
    case 500: status_line = "HTTP/1.1 500 Internal Server Error\r\n"; status_line_len = 36; break;
    case 501: status_line = "HTTP/1.1 501 Not Implemented\r\n"; status_line_len = 30; break;
    case 502: status_line = "HTTP/1.1 502 Bad Gateway\r\n"; status_line_len = 26; break;
    case 503: status_line = "HTTP/1.1 503 Service Unavailable\r\n"; status_line_len = 34; break;
    case 504: status_line = "HTTP/1.1 504 Gateway Timeout\r\n"; status_line_len = 30; break;
    case 505: status_line = "HTTP/1.1 505 HTTP Version Not Supported\r\n"; status_line_len = 41; break;
    case 507: status_line = "HTTP/1.1 507 Insufficient Storage\r\n"; status_line_len = 35; break;
    default: status_line = "HTTP/1.1 500 Internal Server Error\r\n"; status_line_len = 36; break;
  }
  RFC822Headers::set_iovec( 0, status_line, status_line_len );
  char date[32];
  Time().as_http_date_time( date, 32 );
  set_header( "Date", date );
  return HTTPMessage::serialize();
}


// http_server.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
class HTTPServer::HTTPResponseTarget : public EventTarget
{
public:
  HTTPResponseTarget( auto_Socket socket_ )
    : socket_( socket_ )
  { }
  // Object
  YIELD_OBJECT_PROTOTYPES( HTTPServer::HTTPResponseTarget, 0 );
  // EventTarget
  void send( Event& ev )
  {
    if ( ev.get_tag() == YIELD_OBJECT_TAG( HTTPResponse ) )
    {
      HTTPResponse& http_response = static_cast<HTTPResponse&>( ev );
      socket_->aio_write( new Socket::AIOWriteControlBlock( http_response.serialize() ) );
      Object::decRef( http_response );
    }
    else
      DebugBreak();
  }
private:
  auto_Socket socket_;
};
class HTTPServer::AIOReadControlBlock : public Socket::AIOReadControlBlock
{
public:
  AIOReadControlBlock( auto_Buffer buffer, auto_HTTPRequest http_request, auto_EventTarget http_request_target )
    : Socket::AIOReadControlBlock( buffer ), http_request( http_request ), http_request_target( http_request_target )
  { }
  void onCompletion( size_t bytes_transferred )
  {
    Socket::AIOReadControlBlock::onCompletion( bytes_transferred );
    for ( ;; )
    {
      ssize_t deserialize_ret = http_request->deserialize( get_buffer() );
      if ( deserialize_ret == 0 )
      {
        http_request->set_response_target( new HTTPResponseTarget( get_socket() ) );
        http_request_target->send( *http_request.release() );
        return;
        // http_request = new HTTPRequest;
        // Try to deserialize the next http_request
      }
      else if ( deserialize_ret > 0 )
      {
        get_socket()->aio_read( new AIOReadControlBlock( new HeapBuffer( 1024 ), http_request, http_request_target ) );
        return;
      }
      else
        DebugBreak();
    }
  }
  void onError( uint32_t )
  {
    get_socket()->close();
  }
private:
  auto_HTTPRequest http_request;
  auto_EventTarget http_request_target;
};
class HTTPServer::AIOAcceptControlBlock : public TCPSocket::AIOAcceptControlBlock
{
public:
  AIOAcceptControlBlock( auto_AIOQueue aio_queue, auto_EventTarget http_request_target )
    : aio_queue( aio_queue ), http_request_target( http_request_target )
  { }
  void onCompletion( size_t )
  {
    get_accepted_tcp_socket()->associate( aio_queue );
    get_accepted_tcp_socket()->aio_read( new AIOReadControlBlock( new HeapBuffer( 1024 ), new HTTPRequest, http_request_target ) );
    static_cast<TCPSocket*>( get_socket().get() )->aio_accept( new AIOAcceptControlBlock( aio_queue, http_request_target ) );
  }
  void onError( uint32_t )
  { }
private:
  auto_AIOQueue aio_queue;
  auto_EventTarget http_request_target;
};
HTTPServer::HTTPServer( auto_AIOQueue aio_queue, auto_EventTarget http_request_target, auto_TCPSocket listen_tcp_socket )
  : aio_queue( aio_queue ), http_request_target( http_request_target ), listen_tcp_socket( listen_tcp_socket )
{
  listen_tcp_socket->associate( aio_queue );
  listen_tcp_socket->aio_accept( new AIOAcceptControlBlock( aio_queue, http_request_target ) );
}
auto_HTTPServer HTTPServer::create( const URI& absolute_uri,
                                    auto_EventTarget http_request_target,
                                    auto_Log log,
                                    auto_SSLContext ssl_context )
{
  auto_SocketAddress sockname = SocketAddress::create( absolute_uri );
  if ( sockname != NULL )
  {
    auto_TCPSocket listen_tcp_socket;
#ifdef YIELD_HAVE_OPENSSL
    if ( absolute_uri.get_scheme() == "https" && ssl_context != NULL )
      listen_tcp_socket = SSLSocket::create( ssl_context ).release();
    else
#endif
      listen_tcp_socket = TCPSocket::create();
    if ( listen_tcp_socket->bind( sockname ) &&
         listen_tcp_socket->listen() )
    {
      auto_AIOQueue aio_queue = AIOQueue::create();
      if ( aio_queue != NULL )
        return new HTTPServer( aio_queue, http_request_target, listen_tcp_socket );
    }
  }
  return NULL;
}


// json_marshaller.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
extern "C"
{
  #include <yajl.h>
};
JSONMarshaller::JSONMarshaller( bool write_empty_strings )
: write_empty_strings( write_empty_strings )
{
  buffer = new StringBuffer;
  root_key = NULL;
  writer = yajl_gen_alloc( NULL );
}
JSONMarshaller::JSONMarshaller( JSONMarshaller& parent_json_marshaller, const char* root_key )
  : buffer( parent_json_marshaller.buffer ), root_key( root_key ),
    write_empty_strings( parent_json_marshaller.write_empty_strings ), writer( parent_json_marshaller.writer )
{ }
JSONMarshaller::~JSONMarshaller()
{
//  if ( root_key == NULL ) // This is the root JSONMarshaller
//    yajl_gen_free( writer );
}
void JSONMarshaller::flushYAJLBuffer()
{
  const unsigned char* buffer;
  unsigned int len;
  yajl_gen_get_buf( writer, &buffer, &len );
  this->buffer->put( buffer, len );
  yajl_gen_clear( writer );
}
void JSONMarshaller::writeBoolean( const char* key, uint32_t, bool value )
{
  writeKey( key );
  yajl_gen_bool( writer, ( int )value );
  flushYAJLBuffer();
}
void JSONMarshaller::writeBuffer( const char*, uint32_t, auto_Buffer )
{
  DebugBreak();
}
void JSONMarshaller::writeKey( const char* key )
{
  if ( in_map && key != NULL )
    yajl_gen_string( writer, reinterpret_cast<const unsigned char*>( key ), static_cast<unsigned int>( strnlen( key, UINT16_MAX ) ) );
}
void JSONMarshaller::writeDouble( const char* key, uint32_t, double value )
{
  writeKey( key );
  yajl_gen_double( writer, value );
  flushYAJLBuffer();
}
void JSONMarshaller::writeInt64( const char* key, uint32_t, int64_t value )
{
  writeKey( key );
  yajl_gen_integer( writer, ( long )value );
  flushYAJLBuffer();
}
void JSONMarshaller::writeMap( const char* key, uint32_t, const Map& value )
{
  writeKey( key );
  JSONMarshaller( *this, key ).writeMap( &value );
}
void JSONMarshaller::writeMap( const Map* value )
{
  yajl_gen_map_open( writer );
  in_map = true;
  if ( value )
    value->marshal( *this );
  yajl_gen_map_close( writer );
  flushYAJLBuffer();
}
void JSONMarshaller::writeStruct( const char* key, uint32_t, const Struct& value )
{
  writeKey( key );
  JSONMarshaller( *this, key ).writeStruct( &value );
}
void JSONMarshaller::writeStruct( const Struct* value )
{
  yajl_gen_map_open( writer );
  in_map = true;
  if ( value )
    value->marshal( *this );
  yajl_gen_map_close( writer );
  flushYAJLBuffer();
}
void JSONMarshaller::writeSequence( const char* key, uint32_t, const Sequence& value )
{
  writeKey( key );
  JSONMarshaller( *this, key ).writeSequence( &value );
}
void JSONMarshaller::writeSequence( const Sequence* value )
{
  yajl_gen_array_open( writer );
  in_map = false;
  if ( value )
    value->marshal( *this );
  yajl_gen_array_close( writer );
  flushYAJLBuffer();
}
void JSONMarshaller::writeString( const char* key, uint32_t, const char* value, size_t value_len )
{
  if ( value_len > 0 || write_empty_strings )
  {
    writeKey( key );
    yajl_gen_string( writer, reinterpret_cast<const unsigned char*>( value ), static_cast<unsigned int>( value_len ) );
    flushYAJLBuffer();
  }
}


// json_unmarshaller.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
extern "C"
{
  #include <yajl.h>
};
class JSONUnmarshaller::JSONValue
{
public:
  JSONValue( auto_Object<StringBuffer> identifier, bool is_map )
    : identifier( identifier ), is_map( is_map )
  {
    parent = child = prev = next = NULL;
    have_read = false;
    as_integer = 0;
  }
  virtual ~JSONValue()
  {
    delete child;
    delete next;
  }
  auto_Object<StringBuffer> identifier;
  bool is_map;
  auto_Object<StringBuffer> as_string;
  union
  {
    double as_double;
    int64_t as_integer;
  };
  JSONValue *parent, *child, *prev, *next;
  bool have_read;
protected:
  JSONValue()
  {
    is_map = true;
    parent = child = prev = next = NULL;
    have_read = false;
    as_integer = 0;
  }
};
class JSONUnmarshaller::JSONObject : public JSONValue
{
public:
  JSONObject( auto_Buffer json_buffer )
  {
    current_json_value = parent_json_value = NULL;
    reader = yajl_alloc( &JSONObject_yajl_callbacks, NULL, this );
    next_map_key = NULL; next_map_key_len = 0;
    const unsigned char* jsonText = static_cast<const unsigned char*>( static_cast<void*>( *json_buffer ) );
    unsigned int jsonTextLength = static_cast<unsigned int>( json_buffer->size() );
    yajl_status yajl_parse_status = yajl_parse( reader, jsonText, jsonTextLength );
    if ( yajl_parse_status == yajl_status_ok )
      return;
    else if ( yajl_parse_status != yajl_status_insufficient_data )
    {
      unsigned char* yajl_error_str = yajl_get_error( reader, 1, jsonText, jsonTextLength );
      std::ostringstream what;
      what << __FILE__ << ":" << __LINE__ << ": JSON parsing error: " << reinterpret_cast<char*>( yajl_error_str ) << std::endl;
      yajl_free_error( yajl_error_str );
      throw Exception( what.str() );
    }
  }
  ~JSONObject()
  {
    yajl_free( reader );
  }
private:
  yajl_handle reader;
  std::string type_name;
  uint32_t tag;
  // Parsing state
  JSONValue *current_json_value, *parent_json_value;
  const char* next_map_key; size_t next_map_key_len;
  // yajl callbacks
  static int handle_yajl_null( void* _self )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    self->createNextJSONValue().as_integer = 0;
    return 1;
  }
  static int handle_yajl_boolean( void* _self, int value )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    self->createNextJSONValue().as_integer = value;
    return 1;
  }
  static int handle_yajl_integer( void* _self, long value )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    self->createNextJSONValue().as_integer = value;
    return 1;
  }
  static int handle_yajl_double( void* _self, double value )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    self->createNextJSONValue().as_double = value;
    return 1;
  }
  static int handle_yajl_string( void* _self, const unsigned char* buffer, unsigned int len )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    JSONValue& json_value = self->createNextJSONValue();
    json_value.as_string = new StringBuffer( reinterpret_cast<const char*>( buffer ), len );
    return 1;
  }
  static int handle_yajl_start_map( void* _self )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    JSONValue& json_value = self->createNextJSONValue( true );
    self->parent_json_value = &json_value;
    self->current_json_value = json_value.child;
    return 1;
  }
  static int handle_yajl_map_key( void* _self, const unsigned char* map_key, unsigned int map_key_len )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    self->next_map_key = reinterpret_cast<const char*>( map_key );
    self->next_map_key_len = map_key_len;
    return 1;
  }
  static int handle_yajl_end_map( void* _self )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    if ( self->current_json_value == NULL ) // Empty map
      self->current_json_value = self->parent_json_value;
    else
      self->current_json_value = self->current_json_value->parent;
    self->parent_json_value = NULL;
    return 1;
  }
  static int handle_yajl_start_array( void* _self )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    JSONValue& json_value = self->createNextJSONValue();
    self->parent_json_value = &json_value;
    self->current_json_value = json_value.child;
    return 1;
  }
  static int handle_yajl_end_array( void* _self )
  {
    JSONObject* self = static_cast<JSONObject*>( _self );
    if ( self->current_json_value == NULL ) // Empty array
      self->current_json_value = self->parent_json_value;
    else
      self->current_json_value = self->current_json_value->parent;
    self->parent_json_value = NULL;
    return 1;
  }
  JSONValue& createNextJSONValue( bool is_map = false )
  {
    auto_Object<StringBuffer> identifier = next_map_key_len != 0 ? new StringBuffer( next_map_key, next_map_key_len ) : NULL;
    next_map_key = NULL; next_map_key_len = 0;
    if ( current_json_value == NULL )
    {
      if ( parent_json_value ) // This is the first value of an array or map
      {
        current_json_value = new JSONValue( identifier, is_map );
        current_json_value->parent = parent_json_value;
        parent_json_value->child = current_json_value;
      }
      else // This is the first value of the whole object
      {
#ifdef _DEBUG
        if ( identifier != NULL ) DebugBreak();
#endif
        current_json_value = this;
      }
    }
    else
    {
      JSONValue* next_json_value = new JSONValue( identifier, is_map );
      next_json_value->parent = current_json_value->parent;
      next_json_value->prev = current_json_value;
      current_json_value->next = next_json_value;
      current_json_value = next_json_value;
    }
    return *current_json_value;
  }
  static yajl_callbacks JSONObject_yajl_callbacks;
};
yajl_callbacks JSONUnmarshaller::JSONObject::JSONObject_yajl_callbacks =
{
  handle_yajl_null,
  handle_yajl_boolean,
  handle_yajl_integer,
  handle_yajl_double,
  NULL,
  handle_yajl_string,
  handle_yajl_start_map,
  handle_yajl_map_key,
  handle_yajl_end_map,
  handle_yajl_start_array,
  handle_yajl_end_array
};
JSONUnmarshaller::JSONUnmarshaller( auto_Buffer buffer )
{
  root_key = NULL;
  root_json_value = new JSONObject( buffer );
  next_json_value = root_json_value->child;
}
JSONUnmarshaller::JSONUnmarshaller( const char* root_key, JSONValue& root_json_value )
  : root_key( root_key ), root_json_value( &root_json_value ),
    next_json_value( root_json_value.child )
{ }
JSONUnmarshaller::~JSONUnmarshaller()
{
//  if ( root_key == NULL )
//    delete root_json_value;
}
bool JSONUnmarshaller::readBoolean( const char* key, uint32_t )
{
  JSONValue* json_value = readJSONValue( key );
  if ( json_value )
  {
    if ( key != NULL ) // Read the value
      return json_value->as_integer != 0;
    else // Read the identifier
      return false; // Doesn't make any sense
  }
  else
    return false;
}
void JSONUnmarshaller::readBuffer( const char*, uint32_t, auto_Buffer value )
{
  DebugBreak();
}
double JSONUnmarshaller::readDouble( const char* key, uint32_t )
{
  JSONValue* json_value = readJSONValue( key );
  if ( json_value )
  {
    if ( key != NULL ) // Read the value
      return json_value->as_double;
    else // Read the identifier
      return atof( json_value->identifier->c_str() );
  }
  else
    return 0;
}
int64_t JSONUnmarshaller::readInt64( const char* key, uint32_t )
{
  JSONValue* json_value = readJSONValue( key );
  if ( json_value )
  {
    if ( key != NULL ) // Read the value
      return json_value->as_integer;
    else // Read the identifier
      return atoi( json_value->identifier->c_str() );
  }
  else
    return 0;
}
void JSONUnmarshaller::readMap( const char* key, uint32_t, Map& value )
{
  JSONValue* json_value;
  if ( key != NULL )
  {
    json_value = readJSONValue( key );
    if ( json_value == NULL )
      return;
  }
  else if ( root_json_value && !root_json_value->have_read )
  {
    if ( root_json_value->is_map )
      json_value = root_json_value;
    else
      return;
  }
  else
    return;
  JSONUnmarshaller child_json_unmarshaller( key, *json_value );
  child_json_unmarshaller.readMap( value );
  json_value->have_read = true;
}
void JSONUnmarshaller::readMap( Map& value )
{
  while ( next_json_value )
    value.unmarshal( *this );
}
void JSONUnmarshaller::readSequence( const char* key, uint32_t, Sequence& value )
{
  JSONValue* json_value;
  if ( key != NULL )
  {
    json_value = readJSONValue( key );
    if ( json_value == NULL )
      return;
  }
  else if ( root_json_value && !root_json_value->have_read )
  {
    if ( !root_json_value->is_map )
      json_value = root_json_value;
    else
      return;
  }
  else
    return;
  JSONUnmarshaller child_json_unmarshaller( key, *json_value );
  child_json_unmarshaller.readSequence( value );
  json_value->have_read = true;
}
void JSONUnmarshaller::readSequence( Sequence& value )
{
  while ( next_json_value )
    value.unmarshal( *this );
}
void JSONUnmarshaller::readString( const char* key, uint32_t, std::string& str )
{
  JSONValue* json_value = readJSONValue( key );
  if ( json_value )
  {
    if ( key != NULL ) // Read the value
    {
      if ( json_value->as_string != NULL )
        str.assign( static_cast<const std::string&>( *json_value->as_string ) );
    }
    else // Read the identifier
      str.assign( static_cast<const std::string&>( *json_value->identifier ) );
  }
}
void JSONUnmarshaller::readStruct( const char* key, uint32_t, Struct& value )
{
  JSONValue* json_value;
  if ( key != NULL )
  {
    json_value = readJSONValue( key );
    if ( json_value == NULL )
      return;
  }
  else if ( root_json_value && !root_json_value->have_read )
  {
    if ( root_json_value->is_map )
      json_value = root_json_value;
    else
      return;
  }
  else
    return;
  JSONUnmarshaller child_json_unmarshaller( key, *json_value );
  child_json_unmarshaller.readStruct( value );
  json_value->have_read = true;
}
void JSONUnmarshaller::readStruct( Struct& s )
{
  s.unmarshal( *this );
}
JSONUnmarshaller::JSONValue* JSONUnmarshaller::readJSONValue( const char* key )
{
  if ( root_json_value->is_map )
  {
    if ( key != NULL ) // Given a key, reading a value
    {
      JSONValue* child_json_value = root_json_value->child;
      while ( child_json_value )
      {
        if ( !child_json_value->have_read && *child_json_value->identifier == key )
        {
          child_json_value->have_read = true;
          return child_json_value;
        }
        child_json_value = child_json_value->next;
      }
    }
    else if ( next_json_value && !next_json_value->have_read ) // Reading the next key
    {
      JSONValue* json_value = next_json_value;
      next_json_value = json_value->next;
      return json_value;
    }
  }
  else
  {
    if ( next_json_value != NULL && !next_json_value->have_read )
    {
      JSONValue* json_value = next_json_value;
      next_json_value = json_value->next;
      json_value->have_read = true;
      return json_value;
    }
  }
  return NULL;
}


// named_pipe.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef _WIN32
#include <windows.h>
#pragma warning( push )
#pragma warning( disable: 4100 )
#endif
auto_NamedPipe NamedPipe::open( const Path& path, uint32_t flags, mode_t mode )
{
#ifdef _WIN32
  Path named_pipe_base_dir_path( TEXT( "\\\\.\\pipe" ) );
  Path named_pipe_path( named_pipe_base_dir_path + path );
  if ( ( flags & O_CREAT ) == O_CREAT ) // Server
  {
    HANDLE hPipe = CreateNamedPipe( named_pipe_path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL );
    if ( hPipe != INVALID_HANDLE_VALUE )
      return new NamedPipe( new File( hPipe ), false );
  }
  else // Client
  {
    auto_File underlying_file = File::open( named_pipe_path, flags );
    if ( underlying_file != NULL )
      return new NamedPipe( underlying_file, true );
  }
#else
  if ( ( flags & O_CREAT ) == O_CREAT )
  {
    if ( ::mkfifo( path, mode ) != -1 )
      flags ^= O_CREAT;
    else
      return NULL;
  }
  auto_File underlying_file = File::open( path, flags );
  if ( underlying_file != NULL )
    return new NamedPipe( underlying_file );
#endif
  return NULL;
}
#ifdef _WIN32
NamedPipe::NamedPipe( auto_File underlying_file, bool connected )
  : underlying_file( underlying_file ), connected( connected )
{ }
#else
NamedPipe::NamedPipe( auto_File underlying_file )
  : underlying_file( underlying_file )
{ }
#endif
#ifdef _WIN32
bool NamedPipe::connect()
{
  if ( connected )
    return true;
  else
  {
    if ( ConnectNamedPipe( *underlying_file, NULL ) != 0 ||
         GetLastError() == ERROR_PIPE_CONNECTED )
    {
      connected = true;
      return true;
    }
    else
      return false;
  }
}
#endif
ssize_t NamedPipe::read( void* buffer, size_t buffer_len )
{
#ifdef _WIN32
  if ( connect() )
    return underlying_file->read( buffer, buffer_len );
  else
    return -1;
#else
  return underlying_file->read( buffer, buffer_len );
#endif
}
ssize_t NamedPipe::write( const void* buffer, size_t buffer_len )
{
#ifdef _WIN32
  if ( connect() )
    return underlying_file->write( buffer, buffer_len );
  else
    return -1;
#else
  return underlying_file->write( buffer, buffer_len );
#endif
}
ssize_t NamedPipe::writev( const iovec* buffers, uint32_t buffers_count )
{
#ifdef _WIN32
  if ( connect() )
    return underlying_file->writev( buffers, buffers_count );
  else
    return -1;
#else
  return underlying_file->writev( buffers, buffers_count );
#endif
}
#ifdef _WIN32
#pragma warning( pop )
#endif


// oncrpc_message.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
template <class ONCRPCMessageType>
ONCRPCMessage<ONCRPCMessageType>::ONCRPCMessage( auto_Interface interface_ )
  : interface_( interface_ )
{
  xid = 0;
  deserialize_state = DESERIALIZING_RECORD_FRAGMENT_MARKER;
}
template <class ONCRPCMessageType>
ONCRPCMessage<ONCRPCMessageType>::ONCRPCMessage( auto_Interface interface_, uint32_t xid, auto_Struct body )
  : interface_( interface_ ), xid( xid ), body( body )
{
  deserialize_state = DESERIALIZING_RECORD_FRAGMENT_MARKER;
}
template <class ONCRPCMessageType>
ONCRPCMessage<ONCRPCMessageType>::~ONCRPCMessage()
{ }
template <class ONCRPCMessageType>
ssize_t ONCRPCMessage<ONCRPCMessageType>::deserialize( auto_Buffer buffer )
{
  switch ( deserialize_state )
  {
    case DESERIALIZING_RECORD_FRAGMENT_MARKER:
    {
      ssize_t deserialize_ret = deserializeRecordFragmentMarker( buffer );
      if ( deserialize_ret == 0 )
        deserialize_state = DESERIALIZING_RECORD_FRAGMENT;
      else
        return deserialize_ret;
    }
    // Drop down
    case DESERIALIZING_RECORD_FRAGMENT:
    {
      ssize_t deserialize_ret = deserializeRecordFragment( buffer );
      if ( deserialize_ret == 0 )
        deserialize_state = DESERIALIZE_DONE;
      else if ( deserialize_ret > 0 )
        deserialize_state = DESERIALIZING_LONG_RECORD_FRAGMENT;
      return deserialize_ret;
    }
    // Drop down
    case DESERIALIZING_LONG_RECORD_FRAGMENT:
    {
      ssize_t deserialize_ret = deserializeLongRecordFragment( buffer );
      if ( deserialize_ret == 0 )
        deserialize_state = DESERIALIZE_DONE;
      else
        return deserialize_ret;
    }
    // Drop down
    case DESERIALIZE_DONE: return 0;
  }
  DebugBreak();
  return -1;
}
template <class ONCRPCMessageType>
ssize_t ONCRPCMessage<ONCRPCMessageType>::deserializeRecordFragmentMarker( auto_Buffer buffer )
{
  uint32_t record_fragment_marker = 0;
  size_t record_fragment_marker_filled = buffer->get( &record_fragment_marker, sizeof( record_fragment_marker ) );
  if ( record_fragment_marker_filled == sizeof( record_fragment_marker ) )
  {
#ifdef __MACH__
    record_fragment_marker = ntohl( record_fragment_marker );
#else
    record_fragment_marker = Machine::ntohl( record_fragment_marker );
#endif
    if ( record_fragment_marker & ( 1 << 31UL ) ) // The highest bit set = last record fragment
    {
      record_fragment_length = record_fragment_marker ^ ( 1 << 31 );
      if ( record_fragment_length > 32 * 1024 * 1024 )
        DebugBreak();
    }
    else
      DebugBreak();
    return 0;
  }
  else if ( record_fragment_marker_filled == 0 )
    return sizeof( record_fragment_marker );
  else
  {
    DebugBreak();
    return sizeof( record_fragment_marker );
  }
}
template <class ONCRPCMessageType>
ssize_t ONCRPCMessage<ONCRPCMessageType>::deserializeRecordFragment( auto_Buffer buffer )
{
  if ( buffer->size() >= record_fragment_length ) // Common case
  {
    record_fragment_buffer = buffer;
    XDRUnmarshaller xdr_unmarshaller( buffer );
    static_cast<ONCRPCMessageType*>( this )->unmarshal( xdr_unmarshaller );
    return 0;
  }
  else
  {
    record_fragment_buffer = new StringBuffer;
    buffer->get( static_cast<std::string&>( *static_cast<StringBuffer*>( record_fragment_buffer.get() ) ),
                 buffer->size() );
    return record_fragment_length - buffer->size();
  }
}
template <class ONCRPCMessageType>
ssize_t ONCRPCMessage<ONCRPCMessageType>::deserializeLongRecordFragment( auto_Buffer buffer )
{
  buffer->get( static_cast<std::string&>( *static_cast<StringBuffer*>( record_fragment_buffer.get() ) ),
               record_fragment_length - record_fragment_buffer->size() );
  if ( record_fragment_buffer->size() == record_fragment_length )
  {
    XDRUnmarshaller xdr_unmarshaller( record_fragment_buffer );
    static_cast<ONCRPCMessageType*>( this )->unmarshal( xdr_unmarshaller );
    return 0;
  }
  else
    return record_fragment_length - record_fragment_buffer->size();
}
template <class ONCRPCMessageType>
void ONCRPCMessage<ONCRPCMessageType>::marshal( Marshaller& marshaller )
{
  marshaller.writeUint32( "xid", 0, xid );
}
template <class ONCRPCMessageType>
auto_Buffer ONCRPCMessage<ONCRPCMessageType>::serialize()
{
  XDRMarshaller xdr_marshaller;
  xdr_marshaller.writeUint32( "record_fragment_marker", 0, 0 );
  static_cast<ONCRPCMessageType*>( this )->marshal( xdr_marshaller );
  auto_StringBuffer xdr_buffer = xdr_marshaller.get_buffer();
  uint32_t record_fragment_length = xdr_buffer->size() - sizeof( uint32_t );
  uint32_t record_fragment_marker = record_fragment_length | ( 1 << 31 ); // Indicate that this is the last fragment
#ifdef __MACH__
  record_fragment_marker = htonl( record_fragment_marker );
#else
  record_fragment_marker = Machine::htonl( record_fragment_marker );
#endif
  static_cast<std::string&>( *xdr_buffer ).replace( 0, sizeof( uint32_t ), reinterpret_cast<const char*>( &record_fragment_marker ), sizeof( uint32_t ) );
  return xdr_buffer.release();
}
template <class ONCRPCMessageType>
void ONCRPCMessage<ONCRPCMessageType>::unmarshal( Unmarshaller& unmarshaller )
{
  xid = unmarshaller.readUint32( "xid", 0 );
}
template class ONCRPCMessage<ONCRPCRequest>;
template class ONCRPCMessage<ONCRPCResponse>;


// oncrpc_request.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
ONCRPCRequest::ONCRPCRequest( auto_Interface interface_ )
  : ONCRPCMessage<ONCRPCRequest>( interface_ )
{ }
ONCRPCRequest::ONCRPCRequest( auto_Interface interface_, auto_Struct body )
  : ONCRPCMessage<ONCRPCRequest>( interface_, static_cast<uint32_t>( Time::getCurrentUnixTimeS() ), body )
{
  prog = 0x20000000 + interface_->get_tag();
  proc = body->get_tag();
  vers = interface_->get_tag();
  credential_auth_flavor = AUTH_NONE;
}
ONCRPCRequest::ONCRPCRequest( auto_Interface interface_, uint32_t credential_auth_flavor, auto_Struct credential, auto_Struct body )
  : ONCRPCMessage<ONCRPCRequest>( interface_, static_cast<uint32_t>( Time::getCurrentUnixTimeS() ), body ),
    credential_auth_flavor( credential_auth_flavor ), credential( credential )
{
  prog = 0x20000000 + interface_->get_tag();
  proc = body->get_tag();
  vers = interface_->get_tag();
}
ONCRPCRequest::ONCRPCRequest( uint32_t prog, uint32_t proc, uint32_t vers, auto_Struct body )
  : ONCRPCMessage<ONCRPCRequest>( NULL, static_cast<uint32_t>( Time::getCurrentUnixTimeS() ), body ),
    prog( prog ), proc( proc ), vers( vers )
{
  credential_auth_flavor = AUTH_NONE;
}
ONCRPCRequest::ONCRPCRequest( uint32_t prog, uint32_t proc, uint32_t vers, uint32_t credential_auth_flavor, auto_Struct credential, auto_Struct body )
  : ONCRPCMessage<ONCRPCRequest>( NULL, static_cast<uint32_t>( Time::getCurrentUnixTimeS() ), body ),
    prog( prog ), proc( proc ), vers( vers ),
    credential_auth_flavor( credential_auth_flavor ), credential( credential )
{ }
auto_Response ONCRPCRequest::createResponse()
{
  return new ONCRPCResponse( get_interface(), get_xid(), static_cast<Request*>( get_body().get() )->createResponse().release() );
}
void ONCRPCRequest::marshal( Marshaller& marshaller )
{
  ONCRPCMessage<ONCRPCRequest>::marshal( marshaller );
  marshaller.writeInt32( "msg_type", 0, 0 ); // MSG_CALL
  marshaller.writeInt32( "rpcvers", 0, 2 );
  marshaller.writeInt32( "prog", 0, prog );
  marshaller.writeInt32( "vers", 0, vers );
  marshaller.writeInt32( "proc", 0, proc );
  marshaller.writeInt32( "credential_auth_flavor", 0, credential_auth_flavor );
  if ( credential_auth_flavor == AUTH_NONE || credential == NULL )
    marshaller.writeInt32( "credential_auth_body_length", 0, 0 );
  else
  {
    XDRMarshaller credential_auth_body_xdr_marshaller;
    credential->marshal( credential_auth_body_xdr_marshaller );
    marshaller.writeBuffer( "credential_auth_body", 0, credential_auth_body_xdr_marshaller.get_buffer().release() );
  }
  marshaller.writeInt32( "verf_auth_flavor", 0, AUTH_NONE );
  marshaller.writeInt32( "verf_auth_body_length", 0, 0 );
  marshaller.writeStruct( "body", 0, *get_body() );
}
void ONCRPCRequest::respond( Response& response )
{
  if ( this->get_response_target() == NULL )
  {
    auto_Interface interface_( get_interface() );
    auto_Struct body( get_body() );
    Request* interface_request = interface_->checkRequest( *body );
    if ( interface_request != NULL )
    {
      if ( response.get_tag() == YIELD_OBJECT_TAG( ONCRPCResponse ) )
      {
        ONCRPCResponse& oncrpc_response = static_cast<ONCRPCResponse&>( response );
        auto_Struct oncrpc_response_body = oncrpc_response.get_body();
        Response* interface_response = interface_->checkResponse( *oncrpc_response_body );
        if ( interface_response != NULL )
        {
          Object::decRef( response );
          return interface_request->respond( interface_response->incRef() );
        }
        else if ( oncrpc_response_body->get_tag() == YIELD_OBJECT_TAG( ExceptionResponse ) )
        {
          Object::decRef( response );
          return interface_request->respond( static_cast<ExceptionResponse&>( *oncrpc_response_body.release() ) );
        }
      }
      else
        return interface_request->respond( response );
    }
  }
  return Request::respond( response );
}
void ONCRPCRequest::unmarshal( Unmarshaller& unmarshaller )
{
  ONCRPCMessage<ONCRPCRequest>::unmarshal( unmarshaller );
  int32_t msg_type = unmarshaller.readInt32( "msg_type", 0 );
  if ( msg_type == 0 ) // CALL
  {
    uint32_t rpcvers = unmarshaller.readUint32( "rpcvers", 0 );
    if ( rpcvers == 2 )
    {
      unmarshaller.readUint32( "prog", 0 );
      unmarshaller.readUint32( "vers", 0 );
      uint32_t proc = unmarshaller.readUint32( "proc", 0 );
      unmarshaller.readUint32( "credential_auth_flavor", 0 );
      std::string credential_auth_body;
      unmarshaller.readString( "credential_auth_body", 0, credential_auth_body );
      unmarshaller.readUint32( "verf_auth_flavor", 0 );
      uint32_t verf_auth_body_length = unmarshaller.readUint32( "credential_auth_body_length", 0 );
      if ( verf_auth_body_length > 0 )
        DebugBreak();
      auto_Struct body( get_body() );
      if ( body != NULL )
        unmarshaller.readStruct( "body", 0, *body );
      else
      {
        body = get_interface()->createRequest( proc ).release();
        if ( body != NULL )
        {
          unmarshaller.readStruct( "body", 0, *body );
          set_body( body );
        }
      }
    }
  }
}


// oncrpc_response.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
ONCRPCResponse::ONCRPCResponse( auto_Interface interface_ )
  : ONCRPCMessage<ONCRPCResponse>( interface_ )
{ }
ONCRPCResponse::ONCRPCResponse( auto_Interface interface_, uint32_t xid, auto_Struct body )
  : ONCRPCMessage<ONCRPCResponse>( interface_, xid, body )
{ }
void ONCRPCResponse::marshal( Marshaller& marshaller )
{
  ONCRPCMessage<ONCRPCResponse>::marshal( marshaller );
  marshaller.writeInt32( "msg_type", 0, 1 ); // MSG_REPLY
  marshaller.writeInt32( "reply_stat", 0, 0 ); // MSG_ACCEPTED
  marshaller.writeInt32( "verf_auth_flavor", 0, 0 );
  marshaller.writeInt32( "verf_authbody_length", 0, 0 );
  auto_Struct body( get_body() );
  if ( body != NULL )
  {
    if ( body->get_tag() != YIELD_OBJECT_TAG( ExceptionResponse ) )
    {
      marshaller.writeInt32( "accept_stat", 0, 0 ); // SUCCESS
      marshaller.writeStruct( "body", 0, *body );
    }
    else
      marshaller.writeInt32( "accept_stat", 0, 5 ); // SYSTEM_ERR
  }
  else
    marshaller.writeInt32( "accept_stat", 0, 5 ); // SYSTEM_ERR
}
void ONCRPCResponse::unmarshal( Unmarshaller& unmarshaller )
{
  ONCRPCMessage<ONCRPCResponse>::unmarshal( unmarshaller );
  auto_Struct body( get_body() );
  int32_t msg_type = unmarshaller.readInt32( "msg_type", 0 );
  if ( msg_type == 1 ) // REPLY
  {
    uint32_t reply_stat = unmarshaller.readUint32( "reply_stat", 0 );
    if ( reply_stat == 0 ) // MSG_ACCEPTED
    {
      uint32_t verf_auth_flavor = unmarshaller.readUint32( "verf_auth_flavor", 0 );
      uint32_t verf_authbody_length = unmarshaller.readUint32( "verf_authbody_length", 0 );
      if ( verf_auth_flavor == 0 && verf_authbody_length == 0 )
      {
        uint32_t accept_stat = unmarshaller.readUint32( "accept_stat", 0 );
        switch ( accept_stat )
        {
          case 0:
          {
            if ( body != NULL )
              unmarshaller.readStruct( "body", 0, *body );
          }
          break;
          case 1: body = new ExceptionResponse( "ONC-RPC exception: program unavailable" ); break;
          case 2: body = new ExceptionResponse( "ONC-RPC exception: program mismatch" ); break;
          case 3: body = new ExceptionResponse( "ONC-RPC exception: procedure unavailable" ); break;
          case 4: body = new ExceptionResponse( "ONC-RPC exception: garbage arguments" ); break;
          case 5: body = new ExceptionResponse( "ONC-RPC exception: system error" ); break;
          default:
          {
            body = get_interface()->createExceptionResponse( accept_stat ).release();
            if ( body != NULL )
              unmarshaller.readStruct( "body", 0, *body );
            else
              body = new ExceptionResponse( "ONC-RPC exception: system error" );
          }
          break;
        }
      }
      else
        body = new ExceptionResponse( "ONC-RPC exception: received unexpected verification body on response" );
    }
    else if ( reply_stat == 1 ) // MSG_REJECTED
      body = new ExceptionResponse( "ONC-RPC exception: received MSG_REJECTED reply_stat" );
    else
      body = new ExceptionResponse( "ONC-RPC exception: received unknown reply_stat" );
  }
  else
    body = new ExceptionResponse( "ONC-RPC exception: received unknown msg_type" );
  set_body( body );
}


// oncrpc_server.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
class ONCRPCServer::ONCRPCResponseTarget : public EventTarget
{
public:
  ONCRPCResponseTarget( auto_Interface interface_, auto_ONCRPCRequest oncrpc_request, auto_SocketAddress peer_sockaddr, auto_Socket socket_ )
    : interface_( interface_ ), oncrpc_request( oncrpc_request ), peer_sockaddr( peer_sockaddr ), socket_( socket_ )
  { }
  // Object
  YIELD_OBJECT_PROTOTYPES( ONCRPCServer::ONCRPCResponseTarget, 0 );
  // EventTarget
  void send( Event& ev )
  {
    ONCRPCResponse oncrpc_response( interface_, oncrpc_request->get_xid(), ev );
    if ( peer_sockaddr != NULL )
      static_cast<UDPSocket*>( socket_.get() )->aio_sendto( new UDPSocket::AIOSendToControlBlock( oncrpc_response.serialize(), peer_sockaddr ) );
    else
      socket_->aio_write( new Socket::AIOWriteControlBlock( oncrpc_response.serialize() ) );
  }
private:
  auto_Interface interface_;
  auto_ONCRPCRequest oncrpc_request;
  auto_SocketAddress peer_sockaddr;
  auto_Socket socket_;
};
class ONCRPCServer::AIOReadControlBlock : public Socket::AIOReadControlBlock
{
public:
  AIOReadControlBlock( auto_Buffer buffer, auto_Interface interface_, auto_ONCRPCRequest oncrpc_request )
    : Socket::AIOReadControlBlock( buffer ), interface_( interface_ ), oncrpc_request( oncrpc_request )
  { }
  void onCompletion( size_t bytes_transferred )
  {
    Socket::AIOReadControlBlock::onCompletion( bytes_transferred );
    for ( ;; )
    {
      ssize_t deserialize_ret = oncrpc_request->deserialize( get_buffer() );
      if ( deserialize_ret == 0 )
      {
        auto_Struct oncrpc_request_body = oncrpc_request->get_body();
        Request* interface_request = interface_->checkRequest( *oncrpc_request_body );
        if ( interface_request != NULL )
        {
          oncrpc_request_body.release();
          interface_request->set_response_target( new ONCRPCResponseTarget( interface_, oncrpc_request, NULL, get_socket() ) );
          interface_->send( *interface_request );
        }
        else
          DebugBreak();
        return;
      }
      else if ( deserialize_ret > 0 )
      {
        get_socket()->aio_read( new AIOReadControlBlock( new HeapBuffer( 1024 ), interface_, oncrpc_request ) );
        return;
      }
      else
        DebugBreak();
    }
  }
  void onError( uint32_t )
  {
    get_socket()->close();
  }
private:
  auto_Interface interface_;
  auto_ONCRPCRequest oncrpc_request;
};
class ONCRPCServer::AIORecvFromControlBlock : public UDPSocket::AIORecvFromControlBlock
{
public:
  AIORecvFromControlBlock( auto_Interface interface_ )
    : UDPSocket::AIORecvFromControlBlock( new HeapBuffer( 1024 ) ),
      interface_( interface_ )
  { }
  // AIOControlBlock
  void onCompletion( size_t bytes_transferred )
  {
    UDPSocket::AIORecvFromControlBlock::onCompletion( bytes_transferred );
    ONCRPCRequest* oncrpc_request = new ONCRPCRequest( interface_ );
    ssize_t deserialize_ret = oncrpc_request->deserialize( get_buffer() );
    if ( deserialize_ret == 0 )
    {
      auto_Struct oncrpc_request_body = oncrpc_request->get_body();
      Request* interface_request = interface_->checkRequest( *oncrpc_request_body );
      if ( interface_request != NULL )
      {
        oncrpc_request_body.release();
        interface_request->set_response_target( new ONCRPCResponseTarget( interface_, oncrpc_request, get_peer_sockaddr(), get_socket() ) );
        interface_->send( *interface_request );
      }
      static_cast<UDPSocket*>( get_socket().get() )->aio_recvfrom( new AIORecvFromControlBlock( interface_) );
    }
    else if ( deserialize_ret < 0 )
      Object::decRef( *oncrpc_request );
    else
      DebugBreak();
  }
  void onError( uint32_t error_code )
  {
    // DebugBreak();
  }
private:
  auto_Interface interface_;
};
class ONCRPCServer::AIOAcceptControlBlock : public TCPSocket::AIOAcceptControlBlock
{
public:
  AIOAcceptControlBlock( auto_AIOQueue aio_queue, auto_Interface interface_ )
    : aio_queue( aio_queue ), interface_( interface_ )
  { }
  void onCompletion( size_t )
  {
    get_accepted_tcp_socket()->associate( aio_queue );
    get_accepted_tcp_socket()->aio_read( new AIOReadControlBlock( new HeapBuffer( 1024 ), interface_, new ONCRPCRequest( interface_ ) ) );
    static_cast<TCPSocket*>( get_socket().get() )->aio_accept( new AIOAcceptControlBlock( aio_queue, interface_ ) );
  }
  void onError( uint32_t )
  { }
private:
  auto_AIOQueue aio_queue;
  auto_Interface interface_;
};
ONCRPCServer::ONCRPCServer( auto_AIOQueue aio_queue, auto_Interface interface_, auto_Socket socket_ )
  : aio_queue( aio_queue ), interface_( interface_ ), socket_( socket_ )
{ }
auto_ONCRPCServer ONCRPCServer::create( const URI& absolute_uri,
                                        auto_Interface interface_,
                                        auto_Log log,
                                        auto_SSLContext ssl_context )
{
  auto_SocketAddress sockname = SocketAddress::create( absolute_uri );
  if ( sockname != NULL )
  {
    auto_AIOQueue aio_queue( AIOQueue::create() );
    if ( aio_queue != NULL )
    {
      if ( absolute_uri.get_scheme() == "oncrpcu" )
      {
        auto_UDPSocket udp_socket( UDPSocket::create() );
        if ( udp_socket != NULL &&
             udp_socket->bind( sockname ) )
        {
          udp_socket->associate( aio_queue );
          udp_socket->aio_recvfrom( new AIORecvFromControlBlock( interface_ ) );
          return new ONCRPCServer( aio_queue, interface_, udp_socket.release() );
        }
      }
      else
      {
        auto_TCPSocket listen_tcp_socket;
#ifdef YIELD_HAVE_OPENSSL
        if ( absolute_uri.get_scheme() == "oncrpcs" && ssl_context != NULL )
          listen_tcp_socket = SSLSocket::create( ssl_context ).release();
        else
#endif
          listen_tcp_socket = TCPSocket::create();
        if ( listen_tcp_socket != NULL &&
             listen_tcp_socket->bind( sockname ) &&
             listen_tcp_socket->listen() )
        {
          listen_tcp_socket->associate( aio_queue );
          listen_tcp_socket->aio_accept( new AIOAcceptControlBlock( aio_queue, interface_ ) );
          return new ONCRPCServer( aio_queue, interface_, listen_tcp_socket.release() );
        }
      }
    }
  }
  return NULL;
}


// pipe.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef _WIN32
#include <windows.h>
#endif
auto_Pipe Pipe::create()
{
#ifdef _WIN32
  SECURITY_ATTRIBUTES pipe_security_attributes;
  pipe_security_attributes.nLength = sizeof( SECURITY_ATTRIBUTES );
  pipe_security_attributes.bInheritHandle = TRUE;
  pipe_security_attributes.lpSecurityDescriptor = NULL;
  void* ends[2];
  if ( CreatePipe( &ends[0], &ends[1], &pipe_security_attributes, 0 ) )
  {
    if ( SetHandleInformation( ends[0], HANDLE_FLAG_INHERIT, 0 ) &&
         SetHandleInformation( ends[1], HANDLE_FLAG_INHERIT, 0 ) )
      return new Pipe( ends );
    else
    {
      CloseHandle( ends[0] );
      CloseHandle( ends[1] );
    }
  }
#else
  int ends[2];
  if ( ::pipe( ends ) != -1 )
    return new Pipe( ends );
#endif
  return NULL;
}
#ifdef _WIN32
Pipe::Pipe( void* ends[2] )
#else
Pipe::Pipe( int ends[2] )
#endif
{
  this->ends[0] = ends[0];
  this->ends[1] = ends[1];
}
ssize_t Pipe::read( void* buffer, size_t buffer_len )
{
#ifdef _WIN32
  DWORD dwBytesRead;
  if ( ::ReadFile( ends[0], buffer, buffer_len, &dwBytesRead, NULL ) )
    return static_cast<ssize_t>( dwBytesRead );
  else
    return -1;
#else
  return ::read( ends[0], buffer, buffer_len );
#endif
}
ssize_t Pipe::write( const void* buffer, size_t buffer_len )
{
#ifdef _WIN32
  DWORD dwBytesWritten;
  if ( ::WriteFile( ends[1], buffer, buffer_len, &dwBytesWritten, NULL ) )
    return static_cast<ssize_t>( dwBytesWritten );
  else
    return -1;
#else
  return ::write( ends[1], buffer, buffer_len );
#endif
}


// process.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h> // For waitpid
#endif
auto_Process Process::create( const Path& command_line )
{
#ifdef _WIN32
  auto_Pipe child_stdin, child_stdout, child_stderr;
  //auto_Object<Pipe> child_stdin = Pipe::create(),
  //                  child_stdout = Pipe::create(),
  //                  child_stderr = Pipe::create();
  STARTUPINFO startup_info;
  ZeroMemory( &startup_info, sizeof( STARTUPINFO ) );
  startup_info.cb = sizeof( STARTUPINFO );
  //startup_info.hStdInput = *child_stdin->get_input_stream()->get_file();
  //startup_info.hStdOutput = *child_stdout->get_output_stream()->get_file();
  //startup_info.hStdError = *child_stdout->get_output_stream()->get_file();
  //startup_info.dwFlags = STARTF_USESTDHANDLES;
  PROCESS_INFORMATION proc_info;
  ZeroMemory( &proc_info, sizeof( PROCESS_INFORMATION ) );
  if ( CreateProcess( NULL, const_cast<wchar_t*>( command_line.get_wide_path().c_str() ) , NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup_info, &proc_info ) )
    return new Process( proc_info.hProcess, proc_info.hThread, child_stdin, child_stdout, child_stderr );
  else
    return NULL;
#else
  const char* argv[] = { static_cast<const char*>( NULL ) };
  return create( command_line, argv );
#endif
}
auto_Process Process::create( int argc, char** argv )
{
  std::vector<char*> argvv;
  for ( int arg_i = 1; arg_i < argc; arg_i++ )
    argvv.push_back( argv[arg_i] );
  argvv.push_back( NULL );
  return create( argv[0], const_cast<const char**>( &argvv[0] ) );
}
auto_Process Process::create( const Path& executable_file_path, const char** null_terminated_argv )
{
#ifdef _WIN32
  const std::string& executable_file_path_str = static_cast<const std::string&>( executable_file_path );
  std::string command_line;
  if ( executable_file_path_str.find( ' ' ) == -1 )
    command_line.append( executable_file_path_str );
  else
  {
    command_line.append( "\"", 1 );
    command_line.append( executable_file_path_str );
    command_line.append( "\"", 1 );
  }
  size_t arg_i = 0;
  while ( null_terminated_argv[arg_i] != NULL )
  {
    command_line.append( " ", 1 );
    command_line.append( null_terminated_argv[arg_i] );
    arg_i++;
  }
  return create( command_line );
#else
  auto_Object<Pipe> child_stdin, child_stdout, child_stderr;
  //auto_Object<Pipe> child_stdin = Pipe::create(),
  //                  child_stdout = Pipe::create(),
  //                  child_stderr = Pipe::create();
  pid_t child_pid = fork();
  if ( child_pid == -1 )
    return NULL;
  else if ( child_pid == 0 ) // Child
  {
    //close( STDIN_FILENO );
    //dup2( *child_stdin->get_input_stream()->get_file(), STDIN_FILENO ); // Set stdin to read end of stdin pipe
    //close( STDOUT_FILENO );
    //dup2( *child_stdout->get_output_stream()->get_file(), STDOUT_FILENO ); // Set stdout to write end of stdout pipe
    //close( STDERR_FILENO );
    //dup2( *child_stderr->get_output_stream()->get_file(), STDERR_FILENO ); // Set stderr to write end of stderr pipe
    std::vector<char*> argv_with_executable_file_path;
    argv_with_executable_file_path.push_back( const_cast<char*>( static_cast<const char*>( executable_file_path ) ) );
    size_t arg_i = 0;
    while ( null_terminated_argv[arg_i] != NULL )
    {
      argv_with_executable_file_path.push_back( const_cast<char*>( null_terminated_argv[arg_i] ) );
      arg_i++;
    }
    argv_with_executable_file_path.push_back( NULL );
    execv( executable_file_path, &argv_with_executable_file_path[0] );
    return NULL;
  }
  else // Parent
    return new Process( child_pid, child_stdin, child_stdout, child_stderr );
#endif
}
#ifdef _WIN32
Process::Process( HANDLE hChildProcess, HANDLE hChildThread, auto_Object<Pipe> child_stdin, auto_Object<Pipe> child_stdout, auto_Object<Pipe> child_stderr )
  : hChildProcess( hChildProcess ), hChildThread( hChildThread ),
    child_stdin( child_stdin ), child_stdout( child_stdout ), child_stderr( child_stderr )
{ }
#else
Process::Process( pid_t child_pid, auto_Object<Pipe> child_stdin, auto_Object<Pipe> child_stdout, auto_Object<Pipe> child_stderr )
  : child_pid( child_pid ),
    child_stdin( child_stdin ), child_stdout( child_stdout ), child_stderr( child_stderr )
{ }
#endif
Process::~Process()
{
#ifdef _WIN32
  CloseHandle( hChildProcess );
  CloseHandle( hChildThread );
#endif
}
bool Process::kill()
{
#ifdef _WIN32
  return TerminateProcess( hChildProcess, 0 ) == TRUE;
#else
  return ::kill( child_pid, SIGKILL ) == 0;
#endif
}
bool Process::poll( int* out_return_code )
{
#ifdef _WIN32
  if ( WaitForSingleObject( hChildProcess, 0 ) != WAIT_TIMEOUT )
  {
    if ( out_return_code )
    {
      DWORD dwChildExitCode;
      GetExitCodeProcess( hChildProcess, &dwChildExitCode );
      *out_return_code = ( int )dwChildExitCode;
    }
    return true;
  }
  else
    return false;
#else
  return waitpid( child_pid, out_return_code, WNOHANG ) >= 0;
#endif
}
bool Process::terminate()
{
#ifdef _WIN32
  return TerminateProcess( hChildProcess, 0 ) == TRUE;
#else
  return ::kill( child_pid, SIGTERM ) == 0;
#endif
}
int Process::wait()
{
#ifdef _WIN32
  WaitForSingleObject( hChildProcess, INFINITE );
  DWORD dwChildExitCode;
  GetExitCodeProcess( hChildProcess, &dwChildExitCode );
  return ( int )dwChildExitCode;
#else
  int stat_loc;
  if ( waitpid( child_pid, &stat_loc, 0 ) >= 0 )
    return stat_loc;
  else
    return -1;
#endif
}


// rfc822_headers.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
RFC822Headers::RFC822Headers( uint8_t reserve_iovecs_count )
{
  deserialize_state = DESERIALIZING_LEADING_WHITESPACE;
  buffer_p = stack_buffer;
  heap_buffer = NULL;
  heap_buffer_len = 0;
  heap_iovecs = NULL;
  for ( uint8_t iovec_i = 0; iovec_i < reserve_iovecs_count; iovec_i++ )
    memset( &stack_iovecs[iovec_i], 0, sizeof( stack_iovecs[iovec_i] ) );
  iovecs_filled = reserve_iovecs_count;
}
RFC822Headers::~RFC822Headers()
{
  delete [] heap_buffer;
}
void RFC822Headers::allocateHeapBuffer()
{
  if ( heap_buffer_len == 0 )
  {
    heap_buffer = new char[512];
    heap_buffer_len = 512;
    memcpy_s( heap_buffer, heap_buffer_len, stack_buffer, buffer_p - stack_buffer );
    buffer_p = heap_buffer + ( buffer_p - stack_buffer );
  }
  else
  {
    heap_buffer_len += 512;
    char* new_heap_buffer = new char[heap_buffer_len];
    memcpy_s( new_heap_buffer, heap_buffer_len, heap_buffer, buffer_p - heap_buffer );
    buffer_p = new_heap_buffer + ( buffer_p - heap_buffer );
    delete [] heap_buffer;
    heap_buffer = new_heap_buffer;
  }
}
ssize_t RFC822Headers::deserialize( auto_Buffer buffer )
{
  for ( ;; )
  {
    switch ( deserialize_state )
    {
      case DESERIALIZING_LEADING_WHITESPACE:
      {
        char c;
        for ( ;; )
        {
          if ( buffer->get( &c, 1 ) == 1 )
          {
            if ( isspace( c ) )
              continue;
            else
            {
              *buffer_p = c;
              buffer_p++; // Don't need to check the end of the buffer here
              deserialize_state = DESERIALIZING_HEADER_NAME;
              break;
            }
          }
          else
            return 1;
        }
      }
      // Fall through
      case DESERIALIZING_HEADER_NAME:
      {
        char c;
        if ( buffer->get( &c, 1 ) == 1 )
        {
          switch ( c )
          {
            case '\r':
            case '\n': deserialize_state = DESERIALIZING_TRAILING_CRLF; continue;
            // TODO: support folded lines here (look for isspace( c ), if so it's an extension of the previous line
            default:
            {
              *buffer_p = c;
              advanceBufferPointer();
              for ( ;; )
              {
                if ( buffer->get( buffer_p, 1 ) )
                {
                  if ( *buffer_p == ':' )
                  {
                    *buffer_p = 0;
                    advanceBufferPointer();
                    deserialize_state = DESERIALIZING_HEADER_NAME_VALUE_SEPARATOR;
                    break;
                  }
                  else
                    advanceBufferPointer();
                }
                else
                  return 1;
              }
            }
            break;
          }
        }
        else
          return 1;
      }
      // Fall through
      case DESERIALIZING_HEADER_NAME_VALUE_SEPARATOR:
      {
        char c;
        for ( ;; )
        {
          if ( buffer->get( &c, 1 ) == 1 )
          {
            if ( isspace( c ) )
              continue;
            else
            {
              *buffer_p = c;
              advanceBufferPointer();
              deserialize_state = DESERIALIZING_HEADER_VALUE;
              break;
            }
          }
          else
            return 1;
        }
      }
      // Fall through
      case DESERIALIZING_HEADER_VALUE:
      {
        for ( ;; )
        {
          if ( buffer->get( buffer_p, 1 ) == 1 )
          {
            if ( *buffer_p == '\r' )
            {
              *buffer_p = 0;
              advanceBufferPointer();
              deserialize_state = DESERIALIZING_HEADER_VALUE_TERMINATOR;
              break;
            }
            else
              advanceBufferPointer();
          }
          else
            return 1;
        }
      }
      // Fall through
      case DESERIALIZING_HEADER_VALUE_TERMINATOR:
      {
        char c;
        for ( ;; )
        {
          if ( buffer->get( &c, 1 ) == 1 )
          {
            if ( c == '\n' )
            {
              deserialize_state = DESERIALIZING_HEADER_NAME;
              break;
            }
          }
          else
            return 1;
        }
      }
      continue; // To the next header name
      case DESERIALIZING_TRAILING_CRLF:
      {
        char c;
        for ( ;; )
        {
          if ( buffer->get( &c, 1 ) == 1 )
          {
            if ( c == '\n' )
            {
              *buffer_p = 0;
              // Fill the iovecs so get_header will work
              // TODO: do this as we're parsing
              const char* temp_buffer_p = heap_buffer ? heap_buffer : stack_buffer;
              while ( temp_buffer_p < buffer_p )
              {
                const char* header_name = temp_buffer_p;
                size_t header_name_len = strnlen( header_name, UINT16_MAX );
                temp_buffer_p += header_name_len + 1;
                const char* header_value = temp_buffer_p;
                size_t header_value_len = strnlen( header_value, UINT16_MAX );
                temp_buffer_p += header_value_len + 1;
                set_next_iovec( header_name, header_name_len );
                set_next_iovec( ": ", 2 );
                set_next_iovec( header_value, header_value_len );
                set_next_iovec( "\r\n", 2 );
              }
              deserialize_state = DESERIALIZE_DONE;
              return 0;
            }
          }
          else
            return 1;
        }
        case DESERIALIZE_DONE: return 0;
      }
    } // switch
  } // for ( ;; )
}
char* RFC822Headers::get_header( const char* header_name, const char* default_value )
{
  struct iovec* iovecs = heap_iovecs != NULL ? heap_iovecs : stack_iovecs;
  for ( uint8_t iovec_i = 0; iovec_i < iovecs_filled; iovec_i += 4 )
  {
    if ( iovecs[iovec_i].iov_len > 0 &&
         strncmp( static_cast<const char*>( iovecs[iovec_i].iov_base ), header_name, iovecs[iovec_i].iov_len ) == 0 )
      return static_cast<char*>( iovecs[iovec_i+2].iov_base );
  }
  return const_cast<char*>( default_value );
}
auto_Buffer RFC822Headers::serialize()
{
  return new GatherBuffer( heap_iovecs != NULL ? heap_iovecs : stack_iovecs, iovecs_filled );
}
//void RFC822Headers::set_header( const char* header, size_t header_len )
//{
//  DebugBreak(); // TODO: Separate header name and value
//  /*
//  if ( header[header_len-1] != '\n' )
//  {
//    copy_iovec( header, header_len );
//    set_next_iovec( "\r\n", 2 );
//  }
//  else
//    copy_iovec( header, header_len );
//    */
//}
void RFC822Headers::set_header( const char* header_name, const char* header_value )
{
  set_next_iovec( header_name, strnlen( header_name, UINT16_MAX ) );
  set_next_iovec( ": ", 2 );
  set_next_iovec( header_value, strnlen( header_value, UINT16_MAX ) );
  set_next_iovec( "\r\n", 2 );
}
void RFC822Headers::set_header( const char* header_name, char* header_value )
{
  set_next_iovec( header_name, strnlen( header_name, UINT16_MAX ) );
  set_next_iovec( ": ", 2 );
  set_next_iovec( header_value, strnlen( header_value, UINT16_MAX ) );
  set_next_iovec( "\r\n", 2 );
}
void RFC822Headers::set_header( char* header_name, char* header_value )
{
  set_next_iovec( header_name, strnlen( header_name, UINT16_MAX ) );
  set_next_iovec( ": ", 2 );
  set_next_iovec( header_value, strnlen( header_value, UINT16_MAX ) );
  set_next_iovec( "\r\n", 2 );
}
void RFC822Headers::set_header( const std::string& header_name, const std::string& header_value )
{
  set_next_iovec( const_cast<char*>( header_name.c_str() ), header_name.size() ); // Copy
  set_next_iovec( ": ", 2 );
  set_next_iovec( const_cast<char*>( header_value.c_str() ), header_value.size() ); // Copy
  set_next_iovec( "\r\n", 2 );
}
void RFC822Headers::set_iovec( uint8_t iovec_i, const char* data, size_t len )
{
  struct iovec _iovec;
  _iovec.iov_base = const_cast<char*>( data );
  _iovec.iov_len = len;
  if ( heap_iovecs == NULL )
  {
    stack_iovecs[iovec_i].iov_base = const_cast<char*>( data );
    stack_iovecs[iovec_i].iov_len = len;
  }
  else
  {
    heap_iovecs[iovec_i].iov_base = const_cast<char*>( data );
    heap_iovecs[iovec_i].iov_len = len;
  }
}
void RFC822Headers::set_next_iovec( char* data, size_t len )
{
  if ( heap_buffer == NULL )
  {
    if ( ( buffer_p + len - stack_buffer ) > YIELD_RFC822_HEADERS_STACK_BUFFER_LENGTH )
    {
      heap_buffer = new char[len];
      heap_buffer_len = len;
      // Don't need to copy anything from the stack buffer or change pointers, since we're not deleting that memory or parsing over it again
      buffer_p = heap_buffer;
    }
  }
  else if ( static_cast<size_t>( buffer_p + len - heap_buffer ) > heap_buffer_len )
  {
    heap_buffer_len += len;
    char* new_heap_buffer = new char[heap_buffer_len];
    memcpy_s( new_heap_buffer, heap_buffer_len, heap_buffer, buffer_p - heap_buffer );
    // Since we're copying the old heap_buffer and deleting its contents we need to adjust the pointers
    struct iovec* iovecs = ( heap_iovecs == NULL ) ? stack_iovecs : heap_iovecs;
    for ( uint8_t iovec_i = 0; iovec_i < iovecs_filled; iovec_i++ )
    {
      if ( iovecs[iovec_i].iov_base >= heap_buffer && iovecs[iovec_i].iov_base <= buffer_p )
        iovecs[iovec_i].iov_base = new_heap_buffer + ( static_cast<char*>( iovecs[iovec_i].iov_base ) - heap_buffer );
    }
    buffer_p = new_heap_buffer + ( buffer_p - heap_buffer );
    delete [] heap_buffer;
    heap_buffer = new_heap_buffer;
  }
  const char* buffer_p_before = buffer_p;
  memcpy_s( buffer_p, len, data, len );
  buffer_p += len;
  if ( data[len-1] == 0 ) len--;
  set_next_iovec( buffer_p_before, len );
}
void RFC822Headers::set_next_iovec( const char* data, size_t len )
{
  struct iovec _iovec;
  _iovec.iov_base = const_cast<char*>( data );
  _iovec.iov_len = len;
  set_next_iovec( _iovec );
}
void RFC822Headers::set_next_iovec( const struct iovec& iovec )
{
  if ( heap_iovecs == NULL )
  {
    if ( iovecs_filled < YIELD_RFC822_HEADERS_STACK_IOVECS_LENGTH )
      stack_iovecs[iovecs_filled] = iovec;
    else
    {
      heap_iovecs = new struct iovec[UCHAR_MAX];
      memcpy_s( heap_iovecs, sizeof( *heap_iovecs ), stack_iovecs, sizeof( stack_iovecs ) );
      heap_iovecs[iovecs_filled] = iovec;
    }
  }
  else if ( iovecs_filled < UCHAR_MAX )
    heap_iovecs[iovecs_filled] = iovec;
  else
    DebugBreak();
  iovecs_filled++;
}


// socket.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#pragma warning( pop )
#pragma comment( lib, "ws2_32.lib" )
#else
#include <sys/socket.h>
#endif
Socket::Socket( int domain, int type, int protocol, int socket_ )
  : domain( domain ), socket_( socket_ ), type( type ), protocol( protocol )
{
  blocking_mode = true;
}
Socket::~Socket()
{
  close();
}
void Socket::aio_read( auto_Object<AIOReadControlBlock> aio_read_control_block )
{
  aio_read_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    auto_Buffer buffer( aio_read_control_block->get_buffer() );
    WSABUF wsabuf[1];
    wsabuf[0].buf = static_cast<CHAR*>( static_cast<void*>( *buffer ) );
    wsabuf[0].len = buffer->capacity() - buffer->size();
    DWORD dwNumberOfBytesReceived, dwFlags = 0;
    if ( ::WSARecv( socket_, wsabuf, 1, &dwNumberOfBytesReceived, &dwFlags, *aio_read_control_block, NULL ) == 0 ||
         ::WSAGetLastError() == WSA_IO_PENDING )
      aio_read_control_block.release();
    else
      aio_read_control_block->onError( ::WSAGetLastError() );
#else
    aio_queue->submit( aio_read_control_block.release() );
#endif
  }
  else
    aio_read_control_block->execute();
}
void Socket::aio_write( auto_Object<AIOWriteControlBlock> aio_write_control_block )
{
  aio_write_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    auto_Buffer buffer( aio_write_control_block->get_buffer() );
    if ( buffer->get_tag() == YIELD_OBJECT_TAG( GatherBuffer ) )
    {
      DWORD dwNumberOfBytesSent;
      if ( ::WSASend( socket_, reinterpret_cast<WSABUF*>( const_cast<struct iovec*>( static_cast<GatherBuffer*>( buffer.get() )->get_iovecs() ) ), static_cast<GatherBuffer*>( buffer.get() )->get_iovecs_len(), &dwNumberOfBytesSent, 0, *aio_write_control_block, NULL ) == 0 ||
           ::WSAGetLastError() == WSA_IO_PENDING )
        aio_write_control_block.release();
      else
        aio_write_control_block->onError( ::WSAGetLastError() );
    }
    else
    {
      WSABUF wsabuf[1];
      wsabuf[0].buf = static_cast<CHAR*>( static_cast<void*>( *buffer ) );
      wsabuf[0].len = buffer->size();
      DWORD dwNumberOfBytesSent;
      if ( ::WSASend( socket_, wsabuf, 1, &dwNumberOfBytesSent, 0, *aio_write_control_block, NULL ) == 0 ||
           ::WSAGetLastError() == WSA_IO_PENDING )
        aio_write_control_block.release();
      else
        aio_write_control_block->onError( ::WSAGetLastError() );
    }
#else
    aio_queue->submit( aio_write_control_block.release() );
#endif
  }
  else
    aio_write_control_block->execute();
}
void Socket::associate( auto_AIOQueue aio_queue )
{
  this->aio_queue = aio_queue;
  aio_queue->associate( *this );
}
bool Socket::bind( auto_SocketAddress to_sockaddr )
{
  for ( ;; )
  {
    struct sockaddr* name; socklen_t namelen;
    if ( to_sockaddr->as_struct_sockaddr( domain, name, namelen ) )
    {
      if ( ::bind( *this, name, namelen ) != -1 )
        return true;
    }
    if ( domain == AF_INET6 &&
#ifdef _WIN32
        ::WSAGetLastError() == WSAEAFNOSUPPORT )
#else
        errno == EAFNOSUPPORT )
#endif
    {
      close();
      this->domain = AF_INET;
      socket_ = ::socket( AF_INET, type, protocol );
      if ( !blocking_mode )
        set_blocking_mode( false );
    }
    else
      return false;
  }
}
bool Socket::close()
{
#ifdef _WIN32
  return ::closesocket( socket_ ) != SOCKET_ERROR;
#else
  return ::close( socket_ ) != -1;
#endif
}
Socket::ConnectStatus Socket::connect( auto_SocketAddress to_sockaddr )
{
  for ( ;; )
  {
    struct sockaddr* name; socklen_t namelen;
    if ( to_sockaddr->as_struct_sockaddr( domain, name, namelen ) )
    {
      if ( ::connect( *this, name, namelen ) != -1 )
        return CONNECT_STATUS_OK;
      else
      {
#ifdef _WIN32
        switch ( ::WSAGetLastError() )
        {
          case WSAEISCONN: return CONNECT_STATUS_OK;
          case WSAEWOULDBLOCK:
          case WSAEINPROGRESS:
          case WSAEINVAL: return CONNECT_STATUS_WOULDBLOCK;
          case WSAEAFNOSUPPORT:
#else
        switch ( errno )
        {
          case EISCONN: return CONNECT_STATUS_OK;
          case EWOULDBLOCK:
          case EINPROGRESS: return CONNECT_STATUS_WOULDBLOCK;
          case EAFNOSUPPORT:
#endif
          {
            if ( domain == AF_INET6 )
            {
              close();
              domain = AF_INET; // Fall back to IPv4
              socket_ = ::socket( domain, type, protocol );
              if ( !blocking_mode )
                set_blocking_mode( false );
              continue; // Try to connect again
            }
            else
              return CONNECT_STATUS_ERROR;
          }
          break;
          default: return CONNECT_STATUS_ERROR;
        }
      }
    }
    else if ( domain == AF_INET6 )
    {
      close();
      domain = AF_INET; // Fall back to IPv4
      socket_ = ::socket( domain, type, protocol );
      if ( !blocking_mode )
        set_blocking_mode( false );
      continue; // Try to connect again
    }
    else
      return CONNECT_STATUS_ERROR;
  }
}
int Socket::create( int& domain, int type, int protocol )
{
#ifdef _WIN32
  SOCKET socket_ = ::socket( domain, type, protocol );
  if ( socket_ != INVALID_SOCKET )
  {
    if ( domain == AF_INET6 )
    {
      DWORD ipv6only = 0; // Allow dual-mode sockets
      setsockopt( socket_, IPPROTO_IPV6, IPV6_V6ONLY, ( char* )&ipv6only, sizeof( ipv6only ) );
    }
    return ( int )socket_;
  }
  else if ( domain == AF_INET6 && ::WSAGetLastError() == WSAEAFNOSUPPORT )
  {
    domain = AF_INET;
    return ( int )::socket( AF_INET, type, protocol );
  }
  else
    return false;
#else
  int socket_ = ::socket( AF_INET6, type, protocol );
  if ( socket_ != -1 )
    return socket_;
  else if ( domain == AF_INET6 && errno == EAFNOSUPPORT )
  {
    domain = AF_INET;
    return ::socket( AF_INET, type, protocol );
  }
  else
    return -1;
#endif
}
bool Socket::get_blocking_mode() const
{
  return blocking_mode;
}
auto_SocketAddress Socket::getpeername()
{
  struct sockaddr_storage peername_sockaddr_storage;
  memset( &peername_sockaddr_storage, 0, sizeof( peername_sockaddr_storage ) );
  socklen_t peername_sockaddr_storage_len = sizeof( peername_sockaddr_storage );
  if ( ::getpeername( *this, reinterpret_cast<struct sockaddr*>( &peername_sockaddr_storage ), &peername_sockaddr_storage_len ) != -1 )
    return new SocketAddress( peername_sockaddr_storage );
  else
    return NULL;
}
auto_SocketAddress Socket::getsockname()
{
  struct sockaddr_storage sockname_sockaddr_storage;
  memset( &sockname_sockaddr_storage, 0, sizeof( sockname_sockaddr_storage ) );
  socklen_t sockname_sockaddr_storage_len = sizeof( sockname_sockaddr_storage );
  if ( ::getsockname( *this, reinterpret_cast<struct sockaddr*>( &sockname_sockaddr_storage ), &sockname_sockaddr_storage_len ) != -1 )
    return new SocketAddress( sockname_sockaddr_storage );
  else
    return NULL;
}
Socket::operator int() const
{
  return socket_;
}
ssize_t Socket::read( auto_Buffer buffer )
{
  ssize_t read_ret = read( static_cast<void*>( *buffer ), buffer->capacity() - buffer->size() );
  if ( read_ret > 0 )
    buffer->put( static_cast<void*>( *buffer ), read_ret );
  return read_ret;
}
ssize_t Socket::read( void* buffer, size_t buffer_len )
{
#if defined(_WIN32)
  return ::recv( socket_, static_cast<char*>( buffer ), static_cast<int>( buffer_len ), 0 ); // No real advantage to WSARecv on Win32 for one buffer
#elif defined(__linux)
  return ::recv( socket_, buffer, buffer_len, MSG_NOSIGNAL );
#else
  return ::recv( socket_, buffer, buffer_len, 0 );
#endif
}
bool Socket::set_blocking_mode( bool blocking )
{
#ifdef _WIN32
  unsigned long val = blocking ? 0 : 1;
  if ( ioctlsocket( *this, FIONBIO, &val ) != SOCKET_ERROR )
  {
    this->blocking_mode = blocking;
    return true;
  }
  else
    return false;
#else
  int current_fcntl_flags = fcntl( *this, F_GETFL, 0 );
  if ( blocking )
  {
    if ( ( current_fcntl_flags & O_NONBLOCK ) == O_NONBLOCK )
    {
      if ( fcntl( *this, F_SETFL, current_fcntl_flags ^ O_NONBLOCK ) != -1 )
      {
        this->blocking_mode = true;
        return true;
      }
      else
        return false;
    }
    else
      return true;
  }
  else
  {
    if ( fcntl( *this, F_SETFL, current_fcntl_flags | O_NONBLOCK ) != -1 )
    {
      this->blocking_mode = false;
      return true;
    }
    else
      return false;
  }
#endif
}
bool Socket::shutdown()
{
  return true;
}
bool Socket::want_read() const
{
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEWOULDBLOCK || ::WSAGetLastError() == WSA_IO_PENDING;
#else
  return errno == EWOULDBLOCK;
#endif
}
bool Socket::want_write() const
{
#ifdef _WIN32
  return ::WSAGetLastError() == WSAEWOULDBLOCK || ::WSAGetLastError() == WSA_IO_PENDING;
#else
  return errno == EWOULDBLOCK;
#endif
}
ssize_t Socket::write( auto_Buffer buffer )
{
  if ( buffer->get_tag() == YIELD_OBJECT_TAG( GatherBuffer ) )
    return writev( static_cast<GatherBuffer*>( buffer.get() )->get_iovecs(), static_cast<GatherBuffer*>( buffer.get() )->get_iovecs_len() );
  else
    return write( static_cast<void*>( *buffer ), buffer->size() );
}
ssize_t Socket::write( const void* buffer, size_t buffer_len )
{
#ifdef _WIN32
  struct iovec buffers[1];
  buffers[0].iov_base = const_cast<void*>( buffer );
  buffers[0].iov_len = buffer_len;
  return writev( buffers, 1 );
#else
#ifdef __linux
  return ::send( socket_, buffer, buffer_len, MSG_NOSIGNAL );
#else
  return ::send( socket_, buffer, buffer_len, 0 );
#endif
#endif
}
ssize_t Socket::writev( const struct iovec* buffers, uint32_t buffers_count )
{
#ifdef _WIN32
  DWORD dwWrittenLength;
  ssize_t write_ret = ::WSASend( socket_, reinterpret_cast<WSABUF*>( const_cast<struct iovec*>( buffers ) ), buffers_count, &dwWrittenLength, 0, NULL, NULL );
  if ( write_ret >= 0 )
    return static_cast<ssize_t>( dwWrittenLength );
  else
    return write_ret;
#else
  // Use sendmsg instead of writev to pass flags on Linux
  struct msghdr _msghdr;
  memset( &_msghdr, 0, sizeof( _msghdr ) );
  _msghdr.msg_iov = const_cast<iovec*>( buffers );
  _msghdr.msg_iovlen = buffers_count;
#ifdef __linux
  return ::sendmsg( socket_, &_msghdr, MSG_NOSIGNAL ); // MSG_NOSIGNAL = disable SIGPIPE
#else
  return ::sendmsg( socket_, &_msghdr, 0 );
#endif
#endif
}
void Socket::AIOConnectControlBlock::execute()
{
  if ( get_socket()->connect( get_peername() ) == CONNECT_STATUS_OK )
    onCompletion( 0 );
  else
    onError( Exception::get_errno() );
}
void Socket::AIOReadControlBlock::execute()
{
  ssize_t read_ret = get_socket()->read( get_buffer() );
  if ( read_ret > 0 )
    onCompletion( static_cast<size_t>( read_ret ) );
  else
    onError( Exception::get_errno() );
}
void Socket::AIOWriteControlBlock::execute()
{
  ssize_t write_ret = get_socket()->write( get_buffer() );
  if ( write_ret > 0 )
    onCompletion( static_cast<size_t>( write_ret ) );
  else
    onError( Exception::get_errno() );
}


// socket_address.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#pragma warning( pop )
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
SocketAddress::SocketAddress( struct addrinfo& addrinfo_list )
  : addrinfo_list( &addrinfo_list ), _sockaddr_storage( NULL )
{ }
SocketAddress::SocketAddress( const struct sockaddr_storage& _sockaddr_storage )
{
  addrinfo_list = NULL;
  this->_sockaddr_storage = new struct sockaddr_storage;
  memcpy_s( this->_sockaddr_storage, sizeof( *this->_sockaddr_storage ), &_sockaddr_storage, sizeof( _sockaddr_storage ) );
}
auto_SocketAddress SocketAddress::create( const URI& uri )
{
  if ( uri.get_host() == "*" )
    return create( NULL, uri.get_port() );
  else
    return create( uri.get_host().c_str(), uri.get_port() );
}
auto_SocketAddress SocketAddress::create( const char* hostname, uint16_t port )
{
  struct addrinfo* addrinfo_list = getaddrinfo( hostname, port );
  if ( addrinfo_list != NULL )
    return new SocketAddress( *addrinfo_list );
  else
    return NULL;
}
SocketAddress::~SocketAddress()
{
  if ( addrinfo_list != NULL )
    freeaddrinfo( addrinfo_list );
  else if ( _sockaddr_storage != NULL )
    delete _sockaddr_storage;
}
bool SocketAddress::as_struct_sockaddr( int family, struct sockaddr*& out_sockaddr, socklen_t& out_sockaddrlen )
{
  if ( addrinfo_list != NULL )
  {
    struct addrinfo* addrinfo_p = addrinfo_list;
    while ( addrinfo_p != NULL )
    {
      if ( addrinfo_p->ai_family == family )
      {
        out_sockaddr = addrinfo_p->ai_addr;
        out_sockaddrlen = addrinfo_p->ai_addrlen;
        return true;
      }
      else
        addrinfo_p = addrinfo_p->ai_next;
    }
  }
  else if ( _sockaddr_storage->ss_family == family )
  {
    out_sockaddr = reinterpret_cast<struct sockaddr*>( _sockaddr_storage );
    out_sockaddrlen = sizeof( *_sockaddr_storage );
    return true;
  }
#ifdef _WIN32
  ::WSASetLastError( WSAEAFNOSUPPORT );
#else
  errno = EAFNOSUPPORT;
#endif
  return false;
}
struct addrinfo* SocketAddress::getaddrinfo( const char* hostname, uint16_t port )
{
  std::ostringstream servname; // ltoa is not very portable
  servname << port; // servname = decimal port or service name. Great interface, guys.
  struct addrinfo addrinfo_hints;
  memset( &addrinfo_hints, 0, sizeof( addrinfo_hints ) );
  addrinfo_hints.ai_family = AF_UNSPEC;
  if ( hostname == NULL )
    addrinfo_hints.ai_flags |= AI_PASSIVE; // To get INADDR_ANYs
  struct addrinfo* addrinfo_list;
  int getaddrinfo_ret = ::getaddrinfo( hostname, servname.str().c_str(), &addrinfo_hints, &addrinfo_list );
  if ( getaddrinfo_ret == 0 )
    return addrinfo_list;
  else
    return NULL;
}
bool SocketAddress::getnameinfo( std::string& out_hostname, bool numeric ) const
{
  char nameinfo[NI_MAXHOST];
  if ( this->getnameinfo( nameinfo, NI_MAXHOST, numeric ) )
  {
    out_hostname.assign( nameinfo );
    return true;
  }
  else
    return false;
}
bool SocketAddress::getnameinfo( char* out_hostname, uint32_t out_hostname_len, bool numeric ) const
{
  if ( addrinfo_list != NULL )
  {
    struct addrinfo* addrinfo_p = addrinfo_list;
    while ( addrinfo_p != NULL )
    {
      if ( ::getnameinfo( addrinfo_p->ai_addr, addrinfo_p->ai_addrlen, out_hostname, out_hostname_len, NULL, 0, numeric ? NI_NUMERICHOST : 0 ) == 0 )
        return true;
      else
        addrinfo_p = addrinfo_p->ai_next;
    }
    return false;
  }
  else
    return ::getnameinfo( reinterpret_cast<sockaddr*>( _sockaddr_storage ), static_cast<socklen_t>( sizeof( *_sockaddr_storage ) ), out_hostname, out_hostname_len, NULL, 0, numeric ? NI_NUMERICHOST : 0 ) == 0;
}
uint16_t SocketAddress::get_port() const
{
  if ( addrinfo_list != NULL )
  {
    switch ( addrinfo_list->ai_family )
    {
      case AF_INET: return ntohs( reinterpret_cast<struct sockaddr_in*>( addrinfo_list->ai_addr )->sin_port );
      case AF_INET6: return ntohs( reinterpret_cast<struct sockaddr_in6*>( addrinfo_list->ai_addr )->sin6_port );
      default: DebugBreak(); return 0;
    }
  }
  else
  {
    switch ( _sockaddr_storage->ss_family )
    {
      case AF_INET: return ntohs( reinterpret_cast<struct sockaddr_in*>( _sockaddr_storage )->sin_port );
      case AF_INET6: return ntohs( reinterpret_cast<struct sockaddr_in6*>( _sockaddr_storage )->sin6_port );
      default: DebugBreak(); return 0;
    }
  }
}
bool SocketAddress::operator==( const SocketAddress& other ) const
{
  if ( addrinfo_list != NULL )
  {
    if ( other.addrinfo_list != NULL )
    {
      struct addrinfo* addrinfo_p = addrinfo_list;
      while ( addrinfo_p != NULL )
      {
        struct addrinfo* other_addrinfo_p = other.addrinfo_list;
        while ( other_addrinfo_p != NULL )
        {
          if ( addrinfo_p->ai_addrlen == other_addrinfo_p->ai_addrlen &&
               memcmp( addrinfo_p->ai_addr, other_addrinfo_p->ai_addr, addrinfo_p->ai_addrlen ) == 0 &&
               addrinfo_p->ai_family == other_addrinfo_p->ai_family &&
               addrinfo_p->ai_protocol == other_addrinfo_p->ai_protocol &&
               addrinfo_p->ai_socktype == other_addrinfo_p->ai_socktype
             )
               break;
          else
            other_addrinfo_p = other_addrinfo_p->ai_next;
        }
        if ( other_addrinfo_p != NULL ) // i.e. we found the addrinfo in the other's list
          addrinfo_p = addrinfo_p->ai_next;
        else
          return false;
      }
      return true;
    }
    else
      return false;
  }
  else if ( other._sockaddr_storage != NULL )
    return memcmp( _sockaddr_storage, other._sockaddr_storage, sizeof( *_sockaddr_storage ) ) == 0;
  else
    return false;
}


// ssl_context.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef YIELD_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#ifdef _WIN32
#pragma comment( lib, "libeay32.lib" )
#pragma comment( lib, "ssleay32.lib" )
#endif
#endif
#ifdef YIELD_HAVE_OPENSSL
static int pem_password_callback( char *buf, int size, int, void *userdata )
{
  const std::string* pem_password = static_cast<const std::string*>( userdata );
  if ( size > static_cast<int>( pem_password->size() ) )
    size = static_cast<int>( pem_password->size() );
  memcpy_s( buf, size, pem_password->c_str(), size );
  return size;
}
SSLContext::SSLContext( SSL_CTX* ctx )
  : ctx( ctx )
{ }
auto_SSLContext SSLContext::create( SSL_METHOD* method )
{
  SSL_CTX* ctx = createSSL_CTX( method );
  if ( ctx != NULL )
    return new SSLContext( ctx );
  else
    return NULL;
}
auto_SSLContext SSLContext::create( SSL_METHOD* method, const Path& pem_certificate_file_path, const Path& pem_private_key_file_path, const std::string& pem_private_key_passphrase )
{
  SSL_CTX* ctx = createSSL_CTX( method );
  if ( SSL_CTX_use_certificate_file( ctx, pem_certificate_file_path, SSL_FILETYPE_PEM ) > 0 )
  {
    if ( !pem_private_key_passphrase.empty() )
    {
      SSL_CTX_set_default_passwd_cb( ctx, pem_password_callback );
      SSL_CTX_set_default_passwd_cb_userdata( ctx, const_cast<std::string*>( &pem_private_key_passphrase ) );
    }
    if ( SSL_CTX_use_PrivateKey_file( ctx, pem_private_key_file_path, SSL_FILETYPE_PEM ) > 0 )
      return new SSLContext( ctx );
  }
  return NULL;
}
auto_SSLContext SSLContext::create( SSL_METHOD* method, const std::string& pem_certificate_str, const std::string& pem_private_key_str, const std::string& pem_private_key_passphrase )
{
  SSL_CTX* ctx = createSSL_CTX( method );
  BIO* pem_certificate_bio = BIO_new_mem_buf( reinterpret_cast<void*>( const_cast<char*>( pem_certificate_str.c_str() ) ), static_cast<int>( pem_certificate_str.size() ) );
  if ( pem_certificate_bio != NULL )
  {
    X509* cert = PEM_read_bio_X509( pem_certificate_bio, NULL, pem_password_callback, const_cast<std::string*>( &pem_private_key_passphrase ) );
    if ( cert != NULL )
    {
      SSL_CTX_use_certificate( ctx, cert );
      BIO* pem_private_key_bio = BIO_new_mem_buf( reinterpret_cast<void*>( const_cast<char*>( pem_private_key_str.c_str() ) ), static_cast<int>( pem_private_key_str.size() ) );
      if ( pem_private_key_bio != NULL )
      {
        EVP_PKEY* pkey = PEM_read_bio_PrivateKey( pem_private_key_bio, NULL, pem_password_callback, const_cast<std::string*>( &pem_private_key_passphrase ) );
        if ( pkey != NULL )
        {
          SSL_CTX_use_PrivateKey( ctx, pkey );
          BIO_free( pem_certificate_bio );
          BIO_free( pem_private_key_bio );
          return new SSLContext( ctx );
        }
        BIO_free( pem_private_key_bio );
      }
    }
    BIO_free( pem_certificate_bio );
  }
  return NULL;
}
auto_SSLContext SSLContext::create( SSL_METHOD* method, const Path& pkcs12_file_path, const std::string& pkcs12_passphrase )
{
  SSL_CTX* ctx = createSSL_CTX( method );
  BIO* bio = BIO_new_file( pkcs12_file_path, "rb" );
  if ( bio != NULL )
  {
    PKCS12* p12 = d2i_PKCS12_bio( bio, NULL );
    if ( p12 != NULL )
    {
      EVP_PKEY* pkey = NULL;
      X509* cert = NULL;
      STACK_OF( X509 )* ca = NULL;
      if ( PKCS12_parse( p12, pkcs12_passphrase.c_str(), &pkey, &cert, &ca ) )
      {
        if ( pkey != NULL && cert != NULL && ca != NULL )
        {
          SSL_CTX_use_certificate( ctx, cert );
          SSL_CTX_use_PrivateKey( ctx, pkey );
          X509_STORE* store = SSL_CTX_get_cert_store( ctx );
          for ( int i = 0; i < sk_X509_num( ca ); i++ )
          {
            X509* store_cert = sk_X509_value( ca, i );
            X509_STORE_add_cert( store, store_cert );
          }
          BIO_free( bio );
          return new SSLContext( ctx );
        }
      }
    }
    BIO_free( bio );
  }
  return NULL;
}
#else
SSLContext::SSLContext()
{ }
auto_SSLContext SSLContext::create()
{
  return new SSLContext;
}
#endif
SSLContext::~SSLContext()
{
#ifdef YIELD_HAVE_OPENSSL
  SSL_CTX_free( ctx );
#endif
}
#ifdef YIELD_HAVE_OPENSSL
SSL_CTX* SSLContext::createSSL_CTX( SSL_METHOD* method )
{
  SSL_library_init();
  OpenSSL_add_all_algorithms();
  SSL_CTX* ctx = SSL_CTX_new( method );
  if ( ctx != NULL )
  {
#ifdef SSL_OP_NO_TICKET
    SSL_CTX_set_options( ctx, SSL_OP_ALL|SSL_OP_NO_TICKET );
#else
    SSL_CTX_set_options( ctx, SSL_OP_ALL );
#endif
    SSL_CTX_set_verify( ctx, SSL_VERIFY_NONE, NULL );
    return ctx;
  }
  else
    return NULL;
}
#endif


// ssl_socket.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#ifdef YIELD_HAVE_OPENSSL
#ifdef _WIN32
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#pragma warning( pop )
#else
#include <netinet/in.h> // For the IPPROTO_* constants
#endif
auto_SSLSocket SSLSocket::create( auto_SSLContext ctx )
{
  return create( AF_INET6, ctx );
}
auto_SSLSocket SSLSocket::create( int domain, auto_SSLContext ctx )
{
  SSL* ssl = SSL_new( ctx->get_ssl_ctx() );
  if ( ssl != NULL )
  {
    int socket_ = Socket::create( domain, SOCK_STREAM, IPPROTO_TCP );
    if ( socket_ != -1 )
      return new SSLSocket( domain, socket_, ctx, ssl );
    else
      return NULL;
  }
  else
    return NULL;
}
SSLSocket::SSLSocket( int domain, int socket_, auto_SSLContext ctx, SSL* ssl )
  : TCPSocket( domain, socket_ ), ctx( ctx ), ssl( ssl )
{ }
SSLSocket::~SSLSocket()
{
  SSL_free( ssl );
}
auto_TCPSocket SSLSocket::accept()
{
  SSL_set_fd( ssl, *this );
  int peer_socket = static_cast<int>( TCPSocket::_accept() );
  if ( peer_socket != -1 )
  {
    SSL* peer_ssl = SSL_new( ctx->get_ssl_ctx() );
    SSL_set_fd( peer_ssl, peer_socket );
    SSL_set_accept_state( peer_ssl );
    return new SSLSocket( get_domain(), peer_socket, ctx, peer_ssl );
  }
  else
    return NULL;
}
void SSLSocket::aio_accept( auto_Object<AIOAcceptControlBlock> aio_accept_control_block )
{
  aio_accept_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
    aio_queue->submit( aio_accept_control_block.release() );
  else
    aio_accept_control_block->execute();
}
void SSLSocket::aio_connect( auto_Object<AIOConnectControlBlock> aio_connect_control_block )
{
  aio_connect_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
    aio_queue->submit( aio_connect_control_block.release() );
  else
    aio_connect_control_block->execute();
}
void SSLSocket::aio_read( auto_Object<AIOReadControlBlock> aio_read_control_block )
{
  aio_read_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
    aio_queue->submit( aio_read_control_block.release() );
  else
    aio_read_control_block->execute();
}
void SSLSocket::aio_write( auto_Object<AIOWriteControlBlock> aio_write_control_block )
{
  aio_write_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
    aio_queue->submit( aio_write_control_block.release() );
  else
    aio_write_control_block->execute();
}
Socket::ConnectStatus SSLSocket::connect( auto_SocketAddress peer_sockaddr )
{
  ConnectStatus connect_status = TCPSocket::connect( peer_sockaddr );
  if ( connect_status == CONNECT_STATUS_OK )
  {
    SSL_set_fd( ssl, *this );
    SSL_set_connect_state( ssl );
    return CONNECT_STATUS_OK;
  }
  else
    return connect_status;
}
/*
void SSLSocket::info_callback( const SSL* ssl, int where, int ret )
{
  std::ostringstream info;
  int w = where & ~SSL_ST_MASK;
  if ( ( w & SSL_ST_CONNECT ) == SSL_ST_CONNECT ) info << "SSL_connect:";
  else if ( ( w & SSL_ST_ACCEPT ) == SSL_ST_ACCEPT ) info << "SSL_accept:";
  else info << "undefined:";
  if ( ( where & SSL_CB_LOOP ) == SSL_CB_LOOP )
    info << SSL_state_string_long( ssl );
  else if ( ( where & SSL_CB_ALERT ) == SSL_CB_ALERT )
  {
    if ( ( where & SSL_CB_READ ) == SSL_CB_READ )
      info << "read:";
    else
      info << "write:";
    info << "SSL3 alert" << SSL_alert_type_string_long( ret ) << ":" << SSL_alert_desc_string_long( ret );
  }
  else if ( ( where & SSL_CB_EXIT ) == SSL_CB_EXIT )
  {
    if ( ret == 0 )
      info << "failed in " << SSL_state_string_long( ssl );
    else
      info << "error in " << SSL_state_string_long( ssl );
  }
  else
    return;
  reinterpret_cast<SSLSocket*>( SSL_get_app_data( const_cast<SSL*>( ssl ) ) )->log->getStream( Log::LOG_NOTICE ) << "SSLSocket: " << info.str();
}
*/
ssize_t SSLSocket::read( void* buffer, size_t buffer_len )
{
  return SSL_read( ssl, buffer, static_cast<int>( buffer_len ) );
}
bool SSLSocket::shutdown()
{
  if ( SSL_shutdown( ssl ) != -1 )
    return TCPSocket::shutdown();
  else
    return false;
}
bool SSLSocket::want_read() const
{
  return SSL_want_read( ssl ) == 1;
}
bool SSLSocket::want_write() const
{
  return SSL_want_write( ssl ) == 1;
}
ssize_t SSLSocket::write( const void* buffer, size_t buffer_len )
{
  return SSL_write( ssl, buffer, static_cast<int>( buffer_len ) );
}
ssize_t SSLSocket::writev( const struct iovec* buffers, uint32_t buffers_count )
{
  if ( buffers_count == 1 )
    return write( buffers[0].iov_base, buffers[0].iov_len );
  else
  {
    std::string buffer;
    for ( uint32_t buffer_i = 0; buffer_i < buffers_count; buffer_i++ )
      buffer.append( static_cast<const char*>( buffers[buffer_i].iov_base ), buffers[buffer_i].iov_len );
    return write( buffer.c_str(), buffer.size() );
  }
}
#endif


// tcp_socket.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).


#if defined(_WIN32)
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma warning( pop )
#else
#include <netinet/in.h> // For the IPPROTO_* constants
#include <netinet/tcp.h> // For the TCP_* constants
#include <sys/socket.h>
#endif

#include <cstring>


#ifdef _WIN32
void* TCPSocket::lpfnAcceptEx = NULL;
void* TCPSocket::lpfnConnectEx = NULL;
#endif


TCPSocket::TCPSocket( int domain, int socket_ )
  : Socket( domain, SOCK_STREAM, IPPROTO_TCP, socket_ )
{
  partial_write_len = 0;
}

auto_TCPSocket TCPSocket::accept()
{
#ifdef _WIN32
  unsigned int peer_socket = _accept();
  if ( peer_socket != INVALID_SOCKET )
#else
  int peer_socket = _accept();
  if ( peer_socket != -1 )
#endif
    return new TCPSocket( get_domain(), peer_socket );
  else
    return NULL;
}

int TCPSocket::_accept()
{
  sockaddr_storage peer_sockaddr_storage;
  socklen_t peer_sockaddr_storage_len = sizeof( peer_sockaddr_storage );
  return ::accept( *this, ( struct sockaddr* )&peer_sockaddr_storage, &peer_sockaddr_storage_len );
}

void TCPSocket::aio_accept( YIELD::auto_Object<AIOAcceptControlBlock> aio_accept_control_block )
{
  aio_accept_control_block->set_socket( incRef() );

  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    if ( lpfnAcceptEx == NULL )
    {
      GUID GuidAcceptEx = WSAID_ACCEPTEX;
      DWORD dwBytes;
      WSAIoctl( *this, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidAcceptEx, sizeof( GuidAcceptEx ), &lpfnAcceptEx, sizeof( lpfnAcceptEx ), &dwBytes, NULL, NULL );
    }

    aio_accept_control_block->accepted_tcp_socket = TCPSocket::create( get_domain() );
    size_t sizeof_peer_sockaddr = ( get_domain() == AF_INET6 ) ? sizeof( sockaddr_in6 ) : sizeof( sockaddr_in );

    DWORD dwBytesReceived;
    if ( static_cast<LPFN_ACCEPTEX>( lpfnAcceptEx )( *this, *aio_accept_control_block->accepted_tcp_socket, aio_accept_control_block->peer_sockaddr, 0, sizeof_peer_sockaddr + 16, sizeof_peer_sockaddr + 16, &dwBytesReceived, ( LPOVERLAPPED )*aio_accept_control_block ) ||
         ::WSAGetLastError() == WSA_IO_PENDING )
      aio_accept_control_block.release();
    else
      aio_accept_control_block->onError( Exception::get_errno() );
#else
    aio_queue->submit( aio_accept_control_block.release() );
#endif
  }
  else
    aio_accept_control_block->execute();
}

void TCPSocket::aio_connect( YIELD::auto_Object<AIOConnectControlBlock> aio_connect_control_block )
{
  aio_connect_control_block->set_socket( incRef() );

  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    if ( lpfnConnectEx == NULL )
    {
      GUID GuidConnectEx = WSAID_CONNECTEX;
      DWORD dwBytes;
      WSAIoctl( *this, SIO_GET_EXTENSION_FUNCTION_POINTER, &GuidConnectEx, sizeof( GuidConnectEx ), &lpfnConnectEx, sizeof( lpfnConnectEx ), &dwBytes, NULL, NULL );
    }

    for ( ;; )
    {
      struct sockaddr* name; socklen_t namelen;
      if ( aio_connect_control_block->get_peername()->as_struct_sockaddr( get_domain(), name, namelen ) )
      {
        if ( bind( SocketAddress::create( NULL, 0 ) ) )
        {
          DWORD dwBytesSent;
          if ( static_cast<LPFN_CONNECTEX>( lpfnConnectEx )( *this, name, namelen, NULL, 0, &dwBytesSent, *aio_connect_control_block ) ||
               ::WSAGetLastError() == WSA_IO_PENDING )
          {
            aio_connect_control_block.release();
            return;
          }
          else
            break;
        }
        else
          break;
      }
      else if ( get_domain() == AF_INET6 )
      {
        close();
        domain = AF_INET; // Fall back to IPv4
        socket_ = ::socket( domain, get_type(), get_protocol() );
        aio_queue->associate( *this );
        continue; // Try to connect again
      }
      else
        break;
    }

    aio_connect_control_block->onError( Exception::get_errno() );
#else
    aio_queue->submit( aio_connect_control_block.release() );
#endif
  }
  else
    aio_connect_control_block->execute();
}

auto_TCPSocket TCPSocket::create()
{
  return create( AF_INET6 );
}

auto_TCPSocket TCPSocket::create( int domain )
{
  int socket_ = Socket::create( domain, SOCK_STREAM, IPPROTO_TCP );
  if ( socket_ != -1 )
    return new TCPSocket( domain, socket_ );
  else
    return NULL;
}

bool TCPSocket::listen()
{
  int flag = 1;
  setsockopt( *this, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>( &flag ), sizeof( int ) );
  flag = 1;
  setsockopt( *this, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>( &flag ), sizeof( int ) );
  linger lingeropt;
  lingeropt.l_onoff = 1;
  lingeropt.l_linger = 0;
  setsockopt( *this, SOL_SOCKET, SO_LINGER, ( char* )&lingeropt, ( int )sizeof( lingeropt ) );
  return ::listen( *this, SOMAXCONN ) != -1;
}

bool TCPSocket::shutdown()
{
#ifdef _WIN32
  return ::shutdown( *this, SD_BOTH ) == 0;
#else
  return ::shutdown( *this, SHUT_RDWR ) != -1;
#endif
}


void TCPSocket::AIOAcceptControlBlock::execute()
{
  accepted_tcp_socket = static_cast<TCPSocket*>( get_socket().get() )->accept();
  if ( accepted_tcp_socket != NULL )
  {
    onCompletion( 0 );
  }
  else
    onError( Exception::get_errno() );
}


// tracing_socket.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
TracingSocket::TracingSocket( auto_Socket underlying_socket, auto_Log log )
  : Socket( underlying_socket->get_domain(), underlying_socket->get_type(), underlying_socket->get_protocol(), -1 ),
    underlying_socket( underlying_socket ), log( log )
{ }
void TracingSocket::aio_connect( auto_Object<AIOConnectControlBlock> aio_connect_control_block )
{
  underlying_socket->aio_connect( aio_connect_control_block );
}
void TracingSocket::aio_read( auto_Object<AIOReadControlBlock> aio_read_control_block )
{
  underlying_socket->aio_read( aio_read_control_block );
}
void TracingSocket::aio_write( auto_Object<AIOWriteControlBlock> aio_write_control_block )
{
  underlying_socket->aio_write( aio_write_control_block );
}
bool TracingSocket::bind( auto_SocketAddress to_sockaddr )
{
  std::string to_hostname;
  if ( to_sockaddr->getnameinfo( to_hostname ) )
    log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: binding socket #" << ( int )*this << " to " << to_hostname << ".";
  return underlying_socket->bind( to_sockaddr );
}
bool TracingSocket::close()
{
  log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: closing socket #" << ( int )*this << ".";
  return underlying_socket->close();
}
Socket::ConnectStatus TracingSocket::connect( auto_SocketAddress to_sockaddr )
{
  std::string to_hostname;
  if ( to_sockaddr->getnameinfo( to_hostname ) )
    log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: connecting socket #" << ( int )*this << " to " << to_hostname << ".";
  return underlying_socket->connect( to_sockaddr );
}
bool TracingSocket::get_blocking_mode() const
{
  return underlying_socket->get_blocking_mode();
}
auto_SocketAddress TracingSocket::getpeername()
{
  return underlying_socket->getpeername();
}
auto_SocketAddress TracingSocket::getsockname()
{
  return underlying_socket->getsockname();
}
TracingSocket::operator int() const
{
  return underlying_socket->operator int();
}
ssize_t TracingSocket::read( void* buffer, size_t buffer_len )
{
  log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: trying to read " << buffer_len << " bytes from socket #" << ( int )*this << ".";
  ssize_t read_ret = underlying_socket->read( buffer, buffer_len );
  if ( read_ret > 0 )
  {
    log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: read " << read_ret << " bytes from socket #" << ( int )*this << ".";
    log->write( buffer, read_ret, Log::LOG_DEBUG );
    log->write( "\n", Log::LOG_DEBUG );
  }
  else if ( read_ret == 0 || ( !underlying_socket->want_read() && !underlying_socket->want_write() ) )
    log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: lost connection while trying to read socket #" <<  ( int )*this << ".";
  return read_ret;
}
bool TracingSocket::set_blocking_mode( bool blocking )
{
//  log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: setting socket #" << ( int )*this << " to " << ( ( blocking ) ? "blocking mode." : "non-blocking mode." );
  return underlying_socket->set_blocking_mode( blocking );
}
bool TracingSocket::shutdown()
{
//  log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: shutting down socket #" << ( int )*this << ".";
  return underlying_socket->shutdown();
}
bool TracingSocket::want_read() const
{
  bool want_read_ret = underlying_socket->want_read();
  if ( want_read_ret )
    log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: would block on read on socket #" << ( int )*this << ".";
  return want_read_ret;
}
bool TracingSocket::want_write() const
{
  bool want_write_ret = underlying_socket->want_write();
  if ( want_write_ret )
    log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: would block on write on socket #" << ( int )*this << ".";
  return want_write_ret;
}
ssize_t TracingSocket::write( const void* buffer, size_t buffer_len )
{
  log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: trying to write " << buffer_len << " bytes to socket #" << ( int )*this << ".";
  ssize_t write_ret = underlying_socket->write( buffer, buffer_len );
  if ( write_ret >= 0 )
  {
    log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: wrote " << write_ret << " bytes to socket #" << ( int )*this << ".";
    log->write( buffer, write_ret, Log::LOG_DEBUG );
    log->write( "\n", Log::LOG_DEBUG );
  }
  else if ( !underlying_socket->want_read() && !underlying_socket->want_write() )
    log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: lost connection while trying to write to socket #" <<  ( int )*this << ".";
  return write_ret;
}
ssize_t TracingSocket::writev( const struct iovec* buffers, uint32_t buffers_count )
{
  size_t buffers_len = 0;
  for ( uint32_t buffer_i = 0; buffer_i < buffers_count; buffer_i++ )
    buffers_len += buffers[buffer_i].iov_len;
  log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: trying to write " << buffers_len << " bytes to socket #" << ( int )*this << ".";
  ssize_t writev_ret = underlying_socket->writev( buffers, buffers_count );
  if ( writev_ret >= 0 )
  {
    size_t temp_sendmsg_ret = static_cast<size_t>( writev_ret );
    log->getStream( Log::LOG_INFO ) << "yield::TracingSocket: wrote " << writev_ret << " bytes to socket #" << ( int )*this << ".";
    for ( uint32_t buffer_i = 0; buffer_i < buffers_count; buffer_i++ )
    {
      if ( buffers[buffer_i].iov_len <= temp_sendmsg_ret )
      {
        log->write( buffers[buffer_i].iov_base, buffers[buffer_i].iov_len, Log::LOG_DEBUG );
        temp_sendmsg_ret -= buffers[buffer_i].iov_len;
      }
      else
      {
        log->write( buffers[buffer_i].iov_base, temp_sendmsg_ret, Log::LOG_DEBUG );
        break;
      }
    }
    log->write( "\n", Log::LOG_DEBUG );
  }
  else if ( !underlying_socket->want_read() && !underlying_socket->want_write() )
    log->getStream( Log::LOG_DEBUG ) << "yield::TracingSocket: lost connection while trying to write to socket #" <<  ( int )*this << ".";
  return writev_ret;
}


// udp_socket.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
#if defined(_WIN32)
#pragma warning( push )
#pragma warning( disable: 4995 )
#include <ws2tcpip.h>
#pragma warning( pop )
#else
#include <netinet/in.h> // For the IPPROTO_* constants
#include <sys/socket.h>
#endif
UDPSocket::UDPSocket( int domain, int socket_ )
  : Socket( domain, SOCK_DGRAM, IPPROTO_UDP, socket_ )
{ }
void UDPSocket::aio_connect( auto_Object<AIOConnectControlBlock> aio_connect_control_block )
{
  aio_connect_control_block->set_socket( incRef() );
  if ( connect( aio_connect_control_block->get_peername() ) == CONNECT_STATUS_OK )
    aio_connect_control_block->onCompletion( 0 );
  else
    aio_connect_control_block->onError( Exception::get_errno() );
}
void UDPSocket::aio_recvfrom( auto_Object<AIORecvFromControlBlock> aio_recvfrom_control_block )
{
  aio_recvfrom_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    auto_Buffer buffer( aio_recvfrom_control_block->get_buffer() );
    WSABUF wsabuf[1];
    wsabuf[0].buf = static_cast<CHAR*>( static_cast<void*>( *buffer ) );
    wsabuf[0].len = buffer->capacity() - buffer->size();
    DWORD dwNumberOfBytesReceived, dwFlags = 0;
    socklen_t peer_sockaddr_len = sizeof( *aio_recvfrom_control_block->peer_sockaddr );
    if ( ::WSARecvFrom( *this, wsabuf, 1, &dwNumberOfBytesReceived, &dwFlags, reinterpret_cast<struct sockaddr*>( aio_recvfrom_control_block->peer_sockaddr ), &peer_sockaddr_len, *aio_recvfrom_control_block, NULL ) == 0 ||
         ::WSAGetLastError() == WSA_IO_PENDING )
         aio_recvfrom_control_block.release();
    else
      aio_recvfrom_control_block->onError( ::WSAGetLastError() );
#else
    aio_queue->submit( aio_recvfrom_control_block.release() );
#endif
  }
  else
    aio_recvfrom_control_block->execute();
}
void UDPSocket::aio_sendto( auto_Object<AIOSendToControlBlock> aio_sendto_control_block )
{
  aio_sendto_control_block->set_socket( incRef() );
  if ( aio_queue != NULL )
  {
#ifdef _WIN32
    struct sockaddr* peer_sockaddr; socklen_t peer_sockaddr_len;
    if ( aio_sendto_control_block->get_peer_sockaddr()->as_struct_sockaddr( get_domain(), peer_sockaddr, peer_sockaddr_len ) )
    {
      auto_Buffer buffer( aio_sendto_control_block->get_buffer() );
      if ( buffer->get_tag() == YIELD_OBJECT_TAG( GatherBuffer ) )
      {
        DWORD dwNumberOfBytesSent;
        if ( ::WSASendTo( socket_, reinterpret_cast<WSABUF*>( const_cast<struct iovec*>( static_cast<GatherBuffer*>( buffer.get() )->get_iovecs() ) ), static_cast<GatherBuffer*>( buffer.get() )->get_iovecs_len(), &dwNumberOfBytesSent, 0, peer_sockaddr, peer_sockaddr_len, *aio_sendto_control_block, NULL ) == 0 ||
             ::WSAGetLastError() == WSA_IO_PENDING )
          aio_sendto_control_block.release();
        else
          aio_sendto_control_block->onError( ::WSAGetLastError() );
      }
      else
      {
        WSABUF wsabuf[1];
        wsabuf[0].buf = static_cast<CHAR*>( static_cast<void*>( *buffer ) );
        wsabuf[0].len = buffer->size();
        DWORD dwNumberOfBytesSent;
        if ( ::WSASendTo( socket_, wsabuf, 1, &dwNumberOfBytesSent, 0, peer_sockaddr, peer_sockaddr_len, *aio_sendto_control_block, NULL ) == 0 ||
             ::WSAGetLastError() == WSA_IO_PENDING )
          aio_sendto_control_block.release();
        else
          aio_sendto_control_block->onError( ::WSAGetLastError() );
      }
    }
    else
      aio_sendto_control_block->onError( ::WSAGetLastError() );
#else
    aio_queue->submit( aio_sendto_control_block.release() );
#endif
  }
  else
    aio_sendto_control_block->execute();
}
auto_UDPSocket UDPSocket::create()
{
  int domain = AF_INET6;
  int socket_ = Socket::create( domain, SOCK_DGRAM, IPPROTO_UDP );
  if ( socket_ != -1 )
    return new UDPSocket( domain, socket_ );
  else
  {
    domain = AF_INET;
    socket_ = Socket::create( domain, SOCK_DGRAM, IPPROTO_UDP );
    if ( socket_ != -1 )
      return new UDPSocket( domain, socket_ );
    else
      return NULL;
  }
}
ssize_t UDPSocket::recvfrom( auto_Buffer buffer, struct sockaddr_storage& peer_sockaddr )
{
  return recvfrom( static_cast<void*>( *buffer ), buffer->size(), peer_sockaddr );
}
ssize_t UDPSocket::recvfrom( void* buffer, size_t buffer_len, struct sockaddr_storage& peer_sockaddr )
{
  socklen_t peer_sockaddr_len = sizeof( peer_sockaddr );
  return ::recvfrom( *this, static_cast<char*>( buffer ), buffer_len, 0, reinterpret_cast<struct sockaddr*>( &peer_sockaddr ), &peer_sockaddr_len );
}
ssize_t UDPSocket::sendto( auto_Buffer buffer, auto_SocketAddress peer_sockaddr )
{
  return sendto( static_cast<void*>( *buffer ), buffer->size(), peer_sockaddr );
}
ssize_t UDPSocket::sendto( const void* buffer, size_t buffer_len, auto_SocketAddress _peer_sockaddr )
{
  struct sockaddr* peer_sockaddr; socklen_t peer_sockaddr_len;
  if ( _peer_sockaddr->as_struct_sockaddr( get_domain(), peer_sockaddr, peer_sockaddr_len ) )
    return ::sendto( *this, static_cast<const char*>( buffer ), buffer_len, 0, peer_sockaddr, peer_sockaddr_len );
  else
    return -1;
}
UDPSocket::AIORecvFromControlBlock::AIORecvFromControlBlock( auto_Buffer buffer )
  : buffer( buffer )
{
  peer_sockaddr = new sockaddr_storage;
}
UDPSocket::AIORecvFromControlBlock::~AIORecvFromControlBlock()
{
  delete peer_sockaddr;
}
auto_SocketAddress UDPSocket::AIORecvFromControlBlock::get_peer_sockaddr() const
{
  return new SocketAddress( *peer_sockaddr );
}
void UDPSocket::AIORecvFromControlBlock::execute()
{
  ssize_t recvfrom_ret = static_cast<UDPSocket*>( get_socket().get() )->recvfrom( buffer, *peer_sockaddr );
  if ( recvfrom_ret > 0 )
    onCompletion( static_cast<size_t>( recvfrom_ret ) );
  else
    onError( Exception::get_errno() );
}
void UDPSocket::AIORecvFromControlBlock::onCompletion( size_t bytes_transferred )
{
  buffer->put( NULL, bytes_transferred );
}
void UDPSocket::AIOSendToControlBlock::execute()
{
  ssize_t sendto_ret = static_cast<UDPSocket*>( get_socket().get() )->sendto( buffer, peer_sockaddr );
  if ( sendto_ret >= 0 )
    onCompletion( static_cast<size_t>( sendto_ret ) );
  else
    onError( Exception::get_errno() );
}


// uri.cpp
// Copyright 2003-2009 Minor Gordon, with original implementations and ideas contributed by Felix Hupfeld.
// This source comes from the Yield project. It is licensed under the GPLv2 (see COPYING for terms and conditions).
extern "C"
{
  #include <uriparser.h>
};
URI::URI( const URI& other )
: scheme( other.scheme ), user( other.user ), password( other.password ),
  host( other.host ), port( other.port ), resource( other.resource )
{ }
auto_URI URI::parse( const char* uri, size_t uri_len )
{
  UriParserStateA parser_state;
  UriUriA parsed_uri;
  parser_state.uri = &parsed_uri;
  if ( uriParseUriExA( &parser_state, uri, uri + uri_len ) == URI_SUCCESS )
  {
    URI* uri = new URI( parsed_uri );
    uriFreeUriMembersA( &parsed_uri );
    return uri;
  }
  else
  {
    uriFreeUriMembersA( &parsed_uri );
    return NULL;
  }
}
void URI::init( const char* uri, size_t uri_len )
{
  UriParserStateA parser_state;
  UriUriA parsed_uri;
  parser_state.uri = &parsed_uri;
  if ( uriParseUriExA( &parser_state, uri, uri + uri_len ) == URI_SUCCESS )
  {
    init( parsed_uri );
    uriFreeUriMembersA( &parsed_uri );
  }
  else
  {
    uriFreeUriMembersA( &parsed_uri );
    throw Exception( "invalid URI" );
  }
}
void URI::init( UriUriA& parsed_uri )
{
  scheme.assign( parsed_uri.scheme.first, parsed_uri.scheme.afterLast - parsed_uri.scheme.first );
  host.assign( parsed_uri.hostText.first, parsed_uri.hostText.afterLast - parsed_uri.hostText.first );
  if ( parsed_uri.portText.first != NULL )
    port = static_cast<uint16_t>( strtol( parsed_uri.portText.first, NULL, 0 ) );
  else
    port = 0;
  if ( parsed_uri.userInfo.first != NULL )
  {
    const char* userInfo_p = parsed_uri.userInfo.first;
    while ( userInfo_p < parsed_uri.userInfo.afterLast )
    {
      if ( *userInfo_p == ':' )
      {
        user.assign( parsed_uri.userInfo.first, userInfo_p - parsed_uri.userInfo.first );
        password.assign( userInfo_p + 1, parsed_uri.userInfo.afterLast - userInfo_p - 1 );
        break;
      }
      userInfo_p++;
    }
    if ( user.empty() ) // No colon found => no password, just the user
      user.assign( parsed_uri.userInfo.first, parsed_uri.userInfo.afterLast - parsed_uri.userInfo.first );
  }
  if ( parsed_uri.pathHead != NULL )
  {
    UriPathSegmentA* path_segment = parsed_uri.pathHead;
    do
    {
      resource.append( "/" );
      resource.append( path_segment->text.first, path_segment->text.afterLast - path_segment->text.first );
      path_segment = path_segment->next;
    }
    while ( path_segment != NULL );
    if ( parsed_uri.query.first != NULL )
    {
      resource.append( "?" );
      resource.append( parsed_uri.query.first, parsed_uri.query.afterLast - parsed_uri.query.first );
    }
  }
  else
    resource = "/";
}
URI::operator std::string() const
{
  std::ostringstream oss;
  oss << *this;
  return oss.str();
}

