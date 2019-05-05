/** Implementation of the pqxx::connection_base abstract base class.
 *
 * pqxx::connection_base encapsulates a frontend to backend connection.
 *
 * Copyright (c) 2000-2019, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 */
#include "pqxx/compiler-internal.hxx"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
// Includes for WSAPoll().
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#elif defined(HAVE_POLL)
// Include for poll().
#include <poll.h>
#elif defined(HAVE_SYS_SELECT_H)
// Include for select() on (recent) POSIX systems.
#include <sys/select.h>
#else
// Includes for select() according to various older standards.
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#if defined(HAVE_UNISTD_H)
#include <unistd.h>
#endif
#endif
#if defined(HAVE_SYS_TIME_H)
#include <sys/time.h>
#endif

extern "C"
{
#include "libpq-fe.h"
}

#include "pqxx/binarystring"
#include "pqxx/connection"
#include "pqxx/connection_base"
#include "pqxx/nontransaction"
#include "pqxx/pipeline"
#include "pqxx/result"
#include "pqxx/strconv"
#include "pqxx/transaction"
#include "pqxx/notification"

#include "pqxx/internal/gates/errorhandler-connection.hxx"
#include "pqxx/internal/gates/result-creation.hxx"
#include "pqxx/internal/gates/result-connection.hxx"

using namespace pqxx;
using namespace pqxx::internal;
using namespace pqxx::prepare;


extern "C"
{
// The PQnoticeProcessor that receives an error or warning from libpq and sends
// it to the appropriate connection for processing.
void pqxx_notice_processor(void *conn, const char *msg) noexcept
{
  reinterpret_cast<pqxx::connection_base *>(conn)->process_notice(msg);
}


// There's no way in libpq to disable a connection's notice processor.  So,
// set an inert one to get the same effect.
void inert_notice_processor(void *, const char *) noexcept {}
}


std::string pqxx::encrypt_password(
        const std::string &user, const std::string &password)
{
  std::unique_ptr<char, void (*)(char *)> p{
	PQencryptPassword(password.c_str(), user.c_str()),
        freepqmem_templated<char>};
  return std::string{p.get()};
}


void pqxx::connection_base::init()
{
  m_conn = m_policy.do_startconnect(m_conn);
  if (m_policy.is_ready(m_conn)) activate();
}


pqxx::result pqxx::connection_base::make_result(
	internal::pq::PGresult *rhs,
	const std::string &query)
{
  return gate::result_creation::create(
        rhs,
        query,
        internal::enc_group(encoding_id()));
}


int pqxx::connection_base::backendpid() const noexcept
{
  return m_conn ? PQbackendPID(m_conn) : 0;
}


namespace
{
PQXX_PURE int socket_of(const ::pqxx::internal::pq::PGconn *c) noexcept
{
  return c ? PQsocket(c) : -1;
}
}


int pqxx::connection_base::sock() const noexcept
{
  return socket_of(m_conn);
}


void pqxx::connection_base::activate()
{
  if (m_completed)
  {
    if (is_open()) return;
    else throw broken_connection{"Broken connection."};
  }

  try
  {
    m_completed = true;
    if (not is_open()) throw broken_connection{err_msg()};
    set_up_state();
  }
  catch (const broken_connection &e)
  {
    disconnect();
    throw broken_connection{e.what()};
  }
}


void pqxx::connection_base::simulate_failure()
{
  if (m_conn) m_conn = m_policy.do_disconnect(m_conn);
}


int pqxx::connection_base::protocol_version() const noexcept
{
  return m_conn ? PQprotocolVersion(m_conn) : 0;
}


int pqxx::connection_base::server_version() const noexcept
{
  return m_serverversion;
}


void pqxx::connection_base::set_variable(const std::string &Var,
	const std::string &Value)
{
  if (m_trans.get())
  {
    // We're in a transaction.  The variable should go in there.
    m_trans.get()->set_variable(Var, Value);
  }
  else
  {
    // We're not in a transaction.  Set a session variable.
    if (is_open()) raw_set_var(Var, Value);
  }
}


std::string pqxx::connection_base::get_variable(const std::string &Var)
{
  return m_trans.get() ? m_trans.get()->get_variable(Var) : raw_get_var(Var);
}


std::string pqxx::connection_base::raw_get_var(const std::string &Var)
{
  return exec(("SHOW " + Var).c_str()).at(0).at(0).as(std::string{});
}


/** Set up various parts of logical connection state that may need to be
 * recovered because the physical connection to the database was lost and is
 * being reset, or that may not have been initialized yet.
 */
void pqxx::connection_base::set_up_state()
{
  read_capabilities();

  // The default notice processor in libpq writes to stderr.  Ours does
  // nothing.
  // If the caller registers an error handler, this gets replaced with an
  // error handler that walks down the connection's chain of handlers.  We
  // don't do that by default because there's a danger: libpq may call the
  // notice processor via a result object, even after the connection has been
  // destroyed and the handlers list no longer exists.
  clear_notice_processor();

  internal_set_trace();

  if (not m_receivers.empty())
  {
    std::stringstream restore_query;

    // Pipeline all queries needed to restore receivers, so we can send them
    // over in one go.

// TODO: Do we still need this, now that we no longer support reactivation?
    // Reinstate all active receivers.
    if (not m_receivers.empty())
    {
      std::string Last;
      for (auto &i: m_receivers)
      {
        // m_receivers can handle multiple receivers waiting on the same event;
        // issue just one LISTEN for each event.
        if (i.first != Last)
        {
          restore_query << "LISTEN " << quote_name(i.first) << "; ";
          Last = i.first;
        }
      }
    }

    // Now do the whole batch at once
    PQsendQuery(m_conn, restore_query.str().c_str());
    result r;
    do
      r = make_result(PQgetResult(m_conn), "[RECONNECT]");
    while (gate::result_connection(r));
  }

  if (not is_open()) throw broken_connection{"Could not connect."};
}


void pqxx::connection_base::check_result(const result &R)
{
  if (not is_open()) throw broken_connection{};

  // A shame we can't quite detect out-of-memory to turn this into a bad_alloc!
  if (not gate::result_connection{R}) throw failure(err_msg());

  gate::result_creation{R}.check_status();
}


void pqxx::connection_base::disconnect() noexcept
{
  m_conn = m_policy.do_disconnect(m_conn);
}


bool pqxx::connection_base::is_open() const noexcept
{
  return m_conn and m_completed and (status() == CONNECTION_OK);
}


void pqxx::connection_base::process_notice_raw(const char msg[]) noexcept
{
  if ((msg == nullptr) or (*msg == '\0')) return;
  const auto
	rbegin = m_errorhandlers.crbegin(),
	rend = m_errorhandlers.crend();
  for (auto i = rbegin; (i != rend) and (**i)(msg); ++i) ;
}


void pqxx::connection_base::process_notice(const char msg[]) noexcept
{
  if (msg == nullptr) return;
  const auto len = strlen(msg);
  if (len == 0) return;
  if (msg[len-1] == '\n')
  {
    process_notice_raw(msg);
  }
  else try
  {
    // Newline is missing.  Try the C++ string version of this function.
    process_notice(std::string{msg});
  }
  catch (const std::exception &)
  {
    // If we can't even do that, use plain old buffer copying instead
    // (unavoidably, this will break up overly long messages!)
    const char separator[] = "[...]\n";
    char buf[1007];
    size_t bytes = sizeof(buf)-sizeof(separator)-1;
    size_t written;
    strcpy(&buf[bytes], separator);
    // Write all chunks but last.  Each will fill the buffer exactly.
    for (written = 0; (written+bytes) < len; written += bytes)
    {
      memcpy(buf, &msg[written], bytes);
      process_notice_raw(buf);
    }
    // Write any remaining bytes (which won't fill an entire buffer)
    bytes = len-written;
    memcpy(buf, &msg[written], bytes);
    // Add trailing nul byte, plus newline unless there already is one
    strcpy(&buf[bytes], &"\n"[buf[bytes-1]=='\n']);
    process_notice_raw(buf);
  }
}


void pqxx::connection_base::process_notice(const std::string &msg) noexcept
{
  // Ensure that message passed to errorhandler ends in newline
  if (msg[msg.size()-1] == '\n')
  {
    process_notice_raw(msg.c_str());
  }
  else try
  {
    const std::string nl = msg + "\n";
    process_notice_raw(nl.c_str());
  }
  catch (const std::exception &)
  {
    // If nothing else works, try writing the message without the newline
    process_notice_raw(msg.c_str());
    // This is ugly.
    process_notice_raw("\n");
  }
}


void pqxx::connection_base::trace(FILE *Out) noexcept
{
  m_trace = Out;
  if (m_conn) internal_set_trace();
}


void pqxx::connection_base::add_receiver(pqxx::notification_receiver *T)
{
  if (T == nullptr) throw argument_error{"Null receiver registered"};

  // Add to receiver list and attempt to start listening.
  const auto p = m_receivers.find(T->channel());
  const receiver_list::value_type NewVal(T->channel(), T);

  if (p == m_receivers.end())
  {
    // Not listening on this event yet, start doing so.
    const std::string LQ("LISTEN " + quote_name(T->channel()));

    if (is_open()) try
    {
      check_result(make_result(PQexec(m_conn, LQ.c_str()), LQ));
    }
    catch (const broken_connection &)
    {
    }
    m_receivers.insert(NewVal);
  }
  else
  {
    m_receivers.insert(p, NewVal);
  }
}


void pqxx::connection_base::remove_receiver(pqxx::notification_receiver *T)
	noexcept
{
  if (T == nullptr) return;

  try
  {
    const std::pair<const std::string, notification_receiver *> needle{
	T->channel(), T};
    auto R = m_receivers.equal_range(needle.first);
    const auto i = find(R.first, R.second, needle);

    if (i == R.second)
    {
      process_notice(
	"Attempt to remove unknown receiver '" + needle.first + "'");
    }
    else
    {
      // Erase first; otherwise a notification for the same receiver may yet
      // come in and wreak havoc.  Thanks Dragan Milenkovic.
      const bool gone = (m_conn and (R.second == ++R.first));
      m_receivers.erase(i);
      if (gone) exec(("UNLISTEN " + quote_name(needle.first)).c_str());
    }
  }
  catch (const std::exception &e)
  {
    process_notice(e.what());
  }
}


bool pqxx::connection_base::consume_input() noexcept
{
  return PQconsumeInput(m_conn) != 0;
}


bool pqxx::connection_base::is_busy() const noexcept
{
  return PQisBusy(m_conn) != 0;
}


namespace
{
/// Stateful libpq "cancel" operation.
class cancel_wrapper
{
  PGcancel *m_cancel;
  char m_errbuf[500];

public:
  explicit cancel_wrapper(PGconn *conn) :
    m_cancel{nullptr},
    m_errbuf{}
  {
    if (conn)
    {
      m_cancel = PQgetCancel(conn);
      if (m_cancel == nullptr) throw std::bad_alloc{};
    }
  }
  ~cancel_wrapper() { if (m_cancel) PQfreeCancel(m_cancel); }

  void operator()()
  {
    if (not m_cancel) return;
    if (PQcancel(m_cancel, m_errbuf, int{sizeof(m_errbuf)}) == 0)
      throw sql_error{std::string{m_errbuf}};
  }
};
}


void pqxx::connection_base::cancel_query()
{
  cancel_wrapper cancel{m_conn};
  cancel();
}


void pqxx::connection_base::set_verbosity(error_verbosity verbosity) noexcept
{
    PQsetErrorVerbosity(m_conn, static_cast<PGVerbosity>(verbosity));
    m_verbosity = verbosity;
}


namespace
{
/// Unique pointer to PGnotify.
using notify_ptr = std::unique_ptr<PGnotify, void (*)(PGnotify *)>;


/// Get one notification from a connection, or null.
notify_ptr get_notif(pqxx::internal::pq::PGconn *conn)
{
  return notify_ptr(PQnotifies(conn), freepqmem_templated<PGnotify>);
}
}


int pqxx::connection_base::get_notifs()
{
  if (not is_open()) return 0;

  if (not consume_input()) throw broken_connection{};

  // Even if somehow we receive notifications during our transaction, don't
  // deliver them.
  if (m_trans.get()) return 0;

  int notifs = 0;
  for (auto N = get_notif(m_conn); N.get(); N = get_notif(m_conn))
  {
    notifs++;

    const auto Hit = m_receivers.equal_range(std::string{N->relname});
    for (auto i = Hit.first; i != Hit.second; ++i) try
    {
      (*i->second)(N->extra, N->be_pid);
    }
    catch (const std::exception &e)
    {
      try
      {
        process_notice(
		"Exception in notification receiver '" +
		i->first +
		"': " +
		e.what() +
		"\n");
      }
      catch (const std::bad_alloc &)
      {
        // Out of memory.  Try to get the message out in a more robust way.
        process_notice(
		"Exception in notification receiver, "
		"and also ran out of memory\n");
      }
      catch (const std::exception &)
      {
        process_notice(
		"Exception in notification receiver "
		"(compounded by other error)\n");
      }
    }

    N.reset();
  }
  return notifs;
}


const char *pqxx::connection_base::dbname() const
{
  if (m_conn == nullptr) throw broken_connection{
	"Can't get database name: connection is inactive."};
  return PQdb(m_conn);
}


const char *pqxx::connection_base::username() const
{
  if (m_conn == nullptr) throw broken_connection{
	"Can't get user name: connection is inactive."};
  return PQuser(m_conn);
}


const char *pqxx::connection_base::hostname() const
{
  if (m_conn == nullptr) throw broken_connection{
	"Can't get server name: connection is inactive."};
  return PQhost(m_conn);
}


const char *pqxx::connection_base::port() const
{
  if (m_conn == nullptr) throw broken_connection{
	"Can't get database port: connection is inactive."};
  return PQport(m_conn);
}


const char *pqxx::connection_base::err_msg() const noexcept
{
  return m_conn ? PQerrorMessage(m_conn) : "No connection to database";
}


void pqxx::connection_base::clear_notice_processor()
{
  PQsetNoticeProcessor(m_conn, inert_notice_processor, nullptr);
}


void pqxx::connection_base::set_notice_processor()
{
  PQsetNoticeProcessor(m_conn, pqxx_notice_processor, this);
}


void pqxx::connection_base::register_errorhandler(errorhandler *handler)
{
  // Set notice processor on demand, i.e. only when the caller actually
  // registers an error handler.
  // We do this just to make it less likely that users fall into the trap
  // where a result object may hold a notice processor derived from its parent
  // connection which has already been destroyed.  Our notice processor goes
  // through the connection's list of error handlers.  If the connection object
  // has already been destroyed though, that list no longer exists.
  // By setting the notice processor on demand, we absolve users who never
  // register an error handler from ahving to care about this nasty subtlety.
  if (m_errorhandlers.empty()) set_notice_processor();
  m_errorhandlers.push_back(handler);
}


void pqxx::connection_base::unregister_errorhandler(errorhandler *handler)
  noexcept
{
  // The errorhandler itself will take care of nulling its pointer to this
  // connection.
  m_errorhandlers.remove(handler);
  if (m_errorhandlers.empty()) clear_notice_processor();
}


std::vector<errorhandler *> pqxx::connection_base::get_errorhandlers() const
{
  return std::vector<errorhandler *>{
    std::begin(m_errorhandlers), std::end(m_errorhandlers)};
}


pqxx::result pqxx::connection_base::exec(const char Query[])
{
  if (m_conn == nullptr) throw broken_connection{
    "Could not execute query: connection is inactive."};

  auto R = make_result(PQexec(m_conn, Query), Query);
  check_result(R);

  get_notifs();
  return R;
}


void pqxx::connection_base::prepare(
	const std::string &name,
	const std::string &definition)
{
  if (m_conn == nullptr) throw broken_connection{
    "Could not prepare statement: connection is inactive."};

  auto r = make_result(
    PQprepare(m_conn, name.c_str(), definition.c_str(), 0, nullptr),
    "[PREPARE " + name + "]");
  check_result(r);
}


void pqxx::connection_base::prepare(const std::string &definition)
{
  this->prepare(std::string{}, definition);
}


void pqxx::connection_base::unprepare(const std::string &name)
{
  exec(("DEALLOCATE " + quote_name(name)).c_str());
}


pqxx::result pqxx::connection_base::exec_prepared(
	const std::string &statement,
	const internal::params &args)
{
  const auto pointers = args.get_pointers();
  const auto pq_result = PQexecPrepared(
	m_conn,
	statement.c_str(),
	int(args.nonnulls.size()),
	pointers.data(),
	args.lengths.data(),
	args.binaries.data(),
	0);
  const auto r = make_result(pq_result, statement);
  check_result(r);
  get_notifs();
  return r;
}


void pqxx::connection_base::close() noexcept
{
  try
  {
    if (m_trans.get())
      process_notice(
	"Closing connection while " + m_trans.get()->description() +
	" is still open.");

    if (not m_receivers.empty())
    {
      process_notice("Closing connection with outstanding receivers.");
      m_receivers.clear();
    }

    std::list<errorhandler *> old_handlers;
    m_errorhandlers.swap(old_handlers);
    const auto
	rbegin = old_handlers.crbegin(),
	rend = old_handlers.crend();
    for (auto i = rbegin; i!=rend; ++i)
      gate::errorhandler_connection_base{**i}.unregister();

    m_conn = m_policy.do_disconnect(m_conn);
  }
  catch (...)
  {
  }
}


void pqxx::connection_base::raw_set_var(
	const std::string &Var,
	const std::string &Value)
{
    exec(("SET " + Var + "=" + Value).c_str());
}


void pqxx::connection_base::internal_set_trace() noexcept
{
  if (m_conn)
  {
    if (m_trace) PQtrace(m_conn, m_trace);
    else PQuntrace(m_conn);
  }
}


int pqxx::connection_base::status() const noexcept
{
  return PQstatus(m_conn);
}


void pqxx::connection_base::register_transaction(transaction_base *T)
{
  m_trans.register_guest(T);
}


void pqxx::connection_base::unregister_transaction(transaction_base *T)
	noexcept
{
  try
  {
    m_trans.unregister_guest(T);
  }
  catch (const std::exception &e)
  {
    process_notice(e.what());
  }
}


bool pqxx::connection_base::read_copy_line(std::string &Line)
{
  if (not is_open())
    throw internal_error{"read_copy_line() without connection"};

  Line.erase();
  bool Result;

  char *Buf = nullptr;
  const std::string query = "[END COPY]";
  const auto line_len = PQgetCopyData(m_conn, &Buf, false);
  switch (line_len)
  {
  case -2:
    throw failure{"Reading of table data failed: " + std::string{err_msg()}};

  case -1:
    for (
	auto R = make_result(PQgetResult(m_conn), query);
        gate::result_connection(R);
	R=make_result(PQgetResult(m_conn), query)
	)
      check_result(R);
    Result = false;
    break;

  case 0:
    throw internal_error{"table read inexplicably went asynchronous"};

  default:
    if (Buf)
    {
      std::unique_ptr<char, void (*)(char *)> PQA(
          Buf, freepqmem_templated<char>);
      Line.assign(Buf, unsigned(line_len));
    }
    Result = true;
  }

  return Result;
}


void pqxx::connection_base::write_copy_line(const std::string &Line)
{
  if (not is_open())
    throw internal_error{"write_copy_line() without connection"};

  const std::string L = Line + '\n';
  const char *const LC = L.c_str();
  const auto Len = L.size();

  if (PQputCopyData(m_conn, LC, int(Len)) <= 0)
  {
    const std::string msg = (
        std::string{"Error writing to table: "} + err_msg());
// TODO: PQendcopy() is documented as obsolete!
    PQendcopy(m_conn);
    throw failure{msg};
  }
}


void pqxx::connection_base::end_copy_write()
{
  int Res = PQputCopyEnd(m_conn, nullptr);
  switch (Res)
  {
  case -1:
    throw failure{"Write to table failed: " + std::string{err_msg()}};
  case 0:
    throw internal_error{"table write is inexplicably asynchronous"};
  case 1:
    // Normal termination.  Retrieve result object.
    break;

  default:
    throw internal_error{
	"unexpected result " + to_string(Res) + " from PQputCopyEnd()"};
  }

  check_result(make_result(PQgetResult(m_conn), "[END COPY]"));
}


void pqxx::connection_base::start_exec(const std::string &Q)
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't execute query: connection is inactive."};
  if (PQsendQuery(m_conn, Q.c_str()) == 0) throw failure{err_msg()};
}


pqxx::internal::pq::PGresult *pqxx::connection_base::get_result()
{
  if (m_conn == nullptr) throw broken_connection{};
  return PQgetResult(m_conn);
}


std::string pqxx::connection_base::esc(const char str[], size_t maxlen) const
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't escape string: connection is not active."};

  std::vector<char> buf(2 * maxlen + 1);
  int err = 0;
  // TODO: Can we make a callback-based string_view alternative to this?
  // TODO: If we can, then quote() can wrap PQescapeLiteral()!
  PQescapeStringConn(m_conn, buf.data(), str, maxlen, &err);
  if (err) throw argument_error{err_msg()};
  return std::string{buf.data()};
}


std::string pqxx::connection_base::esc(const char str[]) const
{
  return this->esc(str, strlen(str));
}


std::string pqxx::connection_base::esc(const std::string &str) const
{
  return this->esc(str.c_str(), str.size());
}


std::string pqxx::connection_base::esc_raw(
        const unsigned char str[],
        size_t len) const
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't escape raw data: connection is not active."};

  size_t bytes = 0;

  std::unique_ptr<unsigned char, void (*)(unsigned char *)> buf{
	PQescapeByteaConn(m_conn, str, len, &bytes),
	freepqmem_templated<unsigned char>};
  if (buf.get() == nullptr) throw std::bad_alloc{};
  return std::string{reinterpret_cast<char *>(buf.get())};
}


std::string pqxx::connection_base::unesc_raw(const char *text) const
{
  size_t len;
  unsigned char *bytes = const_cast<unsigned char *>(
	reinterpret_cast<const unsigned char *>(text));
  const std::unique_ptr<unsigned char, decltype(internal::freepqmem)*> ptr{
    PQunescapeBytea(bytes, &len),
    internal::freepqmem};
  return std::string{ptr.get(), ptr.get() + len};
}


std::string pqxx::connection_base::quote_raw(
        const unsigned char str[],
        size_t len) const
{
  return "'" + esc_raw(str, len) + "'::bytea";
}


std::string pqxx::connection_base::quote(const binarystring &b) const
{
  return quote_raw(b.data(), b.size());
}


std::string pqxx::connection_base::quote_name(const std::string &identifier)
	const
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't escape identifier: connection is not active."};

  std::unique_ptr<char, void (*)(char *)> buf{
	PQescapeIdentifier(m_conn, identifier.c_str(), identifier.size()),
        freepqmem_templated<char>};
  if (buf.get() == nullptr) throw failure{err_msg()};
  return std::string{buf.get()};
}


std::string pqxx::connection_base::esc_like(
	const std::string &str,
	char escape_char) const
{
  std::string out;
  out.reserve(str.size());
  internal::for_glyphs(
	internal::enc_group(encoding_id()),
	[&out, escape_char](const char *gbegin, const char *gend)
	{
	  if ((gend - gbegin == 1) and (*gbegin == '_' or *gbegin == '%'))
	    out.push_back(escape_char);

          for (; gbegin != gend; ++gbegin) out.push_back(*gbegin);
	},
	str.c_str(),
	str.size());
  return out;
}


namespace
{
#if defined(_WIN32) || defined(HAVE_POLL)
// Convert a timeval to milliseconds, or -1 if no timeval is given.
inline int tv_milliseconds(timeval *tv = nullptr)
{
  return tv ? int(tv->tv_sec * 1000 + tv->tv_usec/1000) : -1;
}
#endif


/// Wait for an fd to become free for reading/writing.  Optional timeout.
void wait_fd(int fd, bool forwrite=false, timeval *tv=nullptr)
{
  if (fd < 0) throw pqxx::broken_connection{};

// WSAPoll is available in winsock2.h only for versions of Windows >= 0x0600
#if defined(_WIN32) && (_WIN32_WINNT >= 0x0600)
  const short events = (forwrite ? POLLWRNORM : POLLRDNORM);
  WSAPOLLFD fdarray{SOCKET(fd), events, 0};
  WSAPoll(&fdarray, 1, tv_milliseconds(tv));
#elif defined(HAVE_POLL)
  const short events = short(
        POLLERR|POLLHUP|POLLNVAL | (forwrite?POLLOUT:POLLIN));
  pollfd pfd{fd, events, 0};
  poll(&pfd, 1, tv_milliseconds(tv));
#else
  // No poll()?  Our last option is select().
  fd_set read_fds;
  FD_ZERO(&read_fds);
  if (not forwrite) FD_SET(fd, &read_fds);

  fd_set write_fds;
  FD_ZERO(&write_fds);
  if (forwrite) FD_SET(fd, &write_fds);

  fd_set except_fds;
  FD_ZERO(&except_fds);
  FD_SET(fd, &except_fds);

  select(fd+1, &read_fds, &write_fds, &except_fds, tv);
#endif

  // No need to report errors.  The caller will try to use the file
  // descriptor right after we return, so if the file descriptor is broken,
  // the caller will notice soon enough.
}
} // namespace

void pqxx::internal::wait_read(const internal::pq::PGconn *c)
{
  wait_fd(socket_of(c));
}


void pqxx::internal::wait_read(
	const internal::pq::PGconn *c,
	long seconds,
	long microseconds)
{
  // These are really supposed to be time_t and suseconds_t.  But not all
  // platforms have that type; some use "long" instead, and some 64-bit
  // systems use 32-bit integers here.  So "int" seems to be the only really
  // safe type to use.
  timeval tv = { time_t(seconds), int(microseconds) };
  wait_fd(socket_of(c), false, &tv);
}


void pqxx::internal::wait_write(const internal::pq::PGconn *c)
{
  wait_fd(socket_of(c), true);
}


void pqxx::connection_base::wait_read() const
{
  internal::wait_read(m_conn);
}


void pqxx::connection_base::wait_read(long seconds, long microseconds) const
{
  internal::wait_read(m_conn, seconds, microseconds);
}


int pqxx::connection_base::await_notification()
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't wait for notifications: connection is not active."};
  int notifs = get_notifs();
  if (notifs == 0)
  {
    wait_read();
    notifs = get_notifs();
  }
  return notifs;
}


int pqxx::connection_base::await_notification(long seconds, long microseconds)
{
  if (m_conn == nullptr) throw broken_connection{
    "Can't wait for notifications: connection is not active."};
  int notifs = get_notifs();
  if (notifs == 0)
  {
    wait_read(seconds, microseconds);
    notifs = get_notifs();
  }
  return notifs;
}


void pqxx::connection_base::read_capabilities()
{
  m_serverversion = PQserverVersion(m_conn);
  if (m_serverversion <= 90000)
    throw feature_not_supported{
	"Unsupported server version; 9.0 is the minimum."};

  const auto proto_ver = protocol_version();
  if (proto_ver == 0)
    throw broken_connection{"No connection."};
  if (proto_ver < 3)
    throw feature_not_supported{
        "Unsupported frontend/backend protocol version; 3.0 is the minimum."};
}


std::string pqxx::connection_base::adorn_name(const std::string &n)
{
  const std::string id = to_string(++m_unique_id);
  return n.empty() ? ("x"+id) : (n+"_"+id);
}


std::string pqxx::connection_base::get_client_encoding() const
{
  return internal::name_encoding(encoding_id());
}


void pqxx::connection_base::set_client_encoding(const char encoding[])
{
  const auto retval = PQsetClientEncoding(m_conn, encoding);
  switch (retval)
  {
  case 0:
    // OK.
    break;
  case -1:
    // TODO: Any helpful information we could give here?
    throw failure{"Setting client encoding failed."};
  default:
    throw internal_error{
	"Unexpected result from PQsetClientEncoding: " + to_string(retval)};
  }
}


void pqxx::connection_base::set_client_encoding(const std::string &encoding)
{
  set_client_encoding(encoding.c_str());
}


int pqxx::connection_base::encoding_id() const
{
  const int enc = PQclientEncoding(m_conn);
  if (enc == -1)
  {
    if (not is_open())
      throw broken_connection{
	"Could not obtain client encoding: not connected."};
    throw failure{"Could not obtain client encoding."};
  }
  return enc;
}


pqxx::result pqxx::connection_base::exec_params(
	const std::string &query,
	const internal::params &args)
{
  const auto pointers = args.get_pointers();
  const auto pq_result = PQexecParams(
	m_conn,
	query.c_str(),
	int(args.nonnulls.size()),
	nullptr,
	pointers.data(),
	args.lengths.data(),
	args.binaries.data(),
	0);
  const auto r = make_result(pq_result, query);
  check_result(r);
  get_notifs();
  return r;
}
