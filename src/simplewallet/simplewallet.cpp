// Copyright (c) 2012-2013 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "warnings.h"
PUSH_WARNINGS
DISABLE_VS_WARNINGS(4100)
DISABLE_VS_WARNINGS(4503)
DISABLE_VS_WARNINGS(4244)
DISABLE_VS_WARNINGS(4101)

#include <thread>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include "include_base_utils.h"
#include "common/command_line.h"
#include "common/util.h"
#include "p2p/net_node.h"
#include "currency_protocol/currency_protocol_handler.h"
#include "simplewallet.h"
#include "currency_core/currency_format_utils.h"
#include "storages/http_abstract_invoke.h"
#include "rpc/core_rpc_server_commands_defs.h"
#include "wallet/wallet_rpc_server.h"
#include "version.h"

#if defined(WIN32)
#include <crtdbg.h>
#endif

using namespace std;
using namespace epee;
using namespace currency;
using boost::lexical_cast;
namespace po = boost::program_options;

#define EXTENDED_LOGS_FILE "wallet_details.log"


namespace
{
  const command_line::arg_descriptor<std::string> arg_wallet_file = {"wallet-file", "Use wallet <arg>", ""};
  const command_line::arg_descriptor<std::string> arg_generate_new_wallet = {"generate-new-wallet", "Generate new wallet and save it to <arg> or <address>.wallet by default", ""};
  const command_line::arg_descriptor<std::string> arg_daemon_address = {"daemon-address", "Use daemon instance at <host>:<port>", ""};
  const command_line::arg_descriptor<std::string> arg_daemon_host = {"daemon-host", "Use daemon instance at host <arg> instead of localhost", ""};
  const command_line::arg_descriptor<std::string> arg_password = {"password", "Wallet password", "", true};
  const command_line::arg_descriptor<int> arg_daemon_port = {"daemon-port", "Use daemon instance at port <arg> instead of default", 0};
  const command_line::arg_descriptor<uint32_t> arg_log_level = {"set_log", "", 0, true};
  //const command_line::arg_descriptor<std::string> arg_config_file = { "config", "config file", ""};

  const command_line::arg_descriptor< std::vector<std::string> > arg_command = {"command", ""};

  inline std::string interpret_rpc_response(bool ok, const std::string& status)
  {
    std::string err;
    if (ok)
    {
      if (status == CORE_RPC_STATUS_BUSY)
      {
        err = "daemon is busy. Please try later";
      }
      else if (status != CORE_RPC_STATUS_OK)
      {
        err = status;
      }
    }
    else
    {
      err = "possible lost connection to daemon";
    }
    return err;
  }

  class message_writer
  {
  public:
    message_writer(epee::log_space::console_colors color = epee::log_space::console_color_default, bool bright = false,
      std::string&& prefix = std::string(), int log_level = LOG_LEVEL_2)
      : m_flush(true)
      , m_color(color)
      , m_bright(bright)
      , m_log_level(log_level)
    {
      m_oss << prefix;
    }

    message_writer(message_writer&& rhs)
      : m_flush(std::move(rhs.m_flush))
#if defined(_MSC_VER)
      , m_oss(std::move(rhs.m_oss))
#else
      // GCC bug: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=54316
      , m_oss(rhs.m_oss.str(), ios_base::out | ios_base::ate) 
#endif
      , m_color(std::move(rhs.m_color))
      , m_log_level(std::move(rhs.m_log_level))
    {
      rhs.m_flush = false;
    }

    template<typename T>
    std::ostream& operator<<(const T& val)
    {
      m_oss << val;
      return m_oss;
    }

    ~message_writer()
    {
      if (m_flush)
      {
        m_flush = false;

        LOG_PRINT(m_oss.str(), m_log_level)

        if (epee::log_space::console_color_default == m_color)
        {
          std::cout << m_oss.str();
        }
        else
        {
          epee::log_space::set_console_color(m_color, m_bright);
          std::cout << m_oss.str();
          epee::log_space::reset_console_color();
        }
        std::cout << std::endl;
      }
    }

  private:
    message_writer(message_writer& rhs);
    message_writer& operator=(message_writer& rhs);
    message_writer& operator=(message_writer&& rhs);

  private:
    bool m_flush;
    std::stringstream m_oss;
    epee::log_space::console_colors m_color;
    bool m_bright;
    int m_log_level;
  };

  message_writer success_msg_writer(bool color = false)
  {
    return message_writer(color ? epee::log_space::console_color_green : epee::log_space::console_color_default, false, std::string(), LOG_LEVEL_2);
  }

  message_writer fail_msg_writer()
  {
    return message_writer(epee::log_space::console_color_red, true, "Error: ", LOG_LEVEL_0);
  }
}

bool simple_wallet::init_config(std::string config_file)
{
	//not finished yet
	epee::serialization::load_t_from_json_file(m_config, config_file);
	if (!m_config.wallets_path_name.size())//wallet file name error
	{
		fail_msg_writer() << "error wallet file from json file: " << SIMPLE_WALLET_CONFIG_FILE;
		return false;
	}
	else//correct
	{

	}


	return false;
}
std::string simple_wallet::get_commands_str()
{
  std::stringstream ss;
  ss << "Commands: " << ENDL;
  std::string usage = m_cmd_binder.get_usage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}

bool simple_wallet::help(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  success_msg_writer() << get_commands_str();
  return true;
}

simple_wallet::simple_wallet()
  : m_daemon_port(0)
  , m_refresh_progress_reporter(*this)
{
  m_cmd_binder.set_handler("start_mining", boost::bind(&simple_wallet::start_mining, this, _1), "start_mining <threads_count> - Start mining in daemon");
  m_cmd_binder.set_handler("stop_mining", boost::bind(&simple_wallet::stop_mining, this, _1), "Stop mining in daemon");
  m_cmd_binder.set_handler("refresh", boost::bind(&simple_wallet::refresh, this, _1), "Resynchronize transactions and balance");
  m_cmd_binder.set_handler("balance", boost::bind(&simple_wallet::show_balance, this, _1), "Show current wallet balance");
  m_cmd_binder.set_handler("incoming_transfers", boost::bind(&simple_wallet::show_incoming_transfers, this, _1), "incoming_transfers [available|unavailable] - Show incoming transfers - all of them or filter them by availability");
  m_cmd_binder.set_handler("list_recent_transfers", boost::bind(&simple_wallet::list_recent_transfers, this, _1), "list_recent_transfers - Show recent maximum 1000 transfers");
  m_cmd_binder.set_handler("payments", boost::bind(&simple_wallet::show_payments, this, _1), "payments <payment_id_1> [<payment_id_2> ... <payment_id_N>] - Show payments <payment_id_1>, ... <payment_id_N>");
  m_cmd_binder.set_handler("bc_height", boost::bind(&simple_wallet::show_blockchain_height, this, _1), "Show blockchain height");
  m_cmd_binder.set_handler("transfer", boost::bind(&simple_wallet::transfer, this, _1), "transfer <mixin_count> <addr_1> <amount_1> [<addr_2> <amount_2> ... <addr_N> <amount_N>] [payment_id] - Transfer <amount_1>,... <amount_N> to <address_1>,... <address_N>, respectively. <mixin_count> is the number of transactions yours is indistinguishable from (from 0 to maximum available)");
  m_cmd_binder.set_handler("set_log", boost::bind(&simple_wallet::set_log, this, _1), "set_log <level> - Change current log detalization level, <level> is a number 0-4");
  m_cmd_binder.set_handler("address", boost::bind(&simple_wallet::print_address, this, _1), "Show current wallet public address");
  m_cmd_binder.set_handler("save", boost::bind(&simple_wallet::save, this, _1), "Save wallet synchronized data");
  m_cmd_binder.set_handler("help", boost::bind(&simple_wallet::help, this, _1), "Show this help");
  m_cmd_binder.set_handler("change_password", boost::bind(&simple_wallet::change_password, this, _1), "change_password <old_password> <new_password>");
  m_cmd_binder.set_handler("make_alias", boost::bind(&simple_wallet::make_alias, this, _1), "pay 101 DNC to miners to make an alias. make_alias <alias>  [comment] [viewkey]");

}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::change_password(const std::vector<std::string> &args)
{
	if(args.size() != 2) 
	{
		fail_msg_writer() << "use: change_password <old_password> <new_password>";
		return true;
	}

	bool r = m_wallet->changepassword(args[0],args[1]);
	if(!r)  
	{
		fail_msg_writer() << "change password failed.";
		return true;
	}
	else
	{
		std::cout << "change password successfully." << std::endl;
	}
	return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_log(const std::vector<std::string> &args)
{
  if(args.size() != 1) 
  {
    fail_msg_writer() << "use: set_log <log_level_number_0-4>";
    return true;
  }
  uint16_t l = 0;
  if(!string_tools::get_xtype_from_string(l, args[0]))
  {
    fail_msg_writer() << "wrong number format, use: set_log <log_level_number_0-4>";
    return true;
  }
  if(LOG_LEVEL_4 < l)
  {
    fail_msg_writer() << "wrong number range, use: set_log <log_level_number_0-4>";
    return true;
  }

  log_space::log_singletone::get_set_log_detalisation_level(true, l);
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::ask_wallet_create_if_needed()
{
  std::string wallet_path;

  wallet_path = command_line::input_line(
      "Specify wallet file name (e.g., wallet.dnc). If the wallet doesn't exist, it will be created.\n"
      "Wallet file name: "
  );

  bool keys_file_exists;
  bool wallet_file_exists;
  tools::wallet2::wallet_exists(wallet_path, keys_file_exists, wallet_file_exists);

  // add logic to error out if new wallet requested but named wallet file exists
  if (keys_file_exists || wallet_file_exists)
  {
    if (!m_generate_new.empty())
    {
      fail_msg_writer() << "Attempting to generate wallet, but specified file(s) exist.  Exiting to not risk overwriting.";
      return false;
    }
  }

  bool r;
  if(keys_file_exists)
  {
    m_wallet_file = wallet_path;
    r = true;
  }else
  {
    if(!wallet_file_exists)
    {
      std::cout << "The wallet doesn't exist, generating new one" << std::endl;
      m_generate_new = wallet_path;
      r = true;
    }else
    {
      fail_msg_writer() << "failed to open wallet \"" << wallet_path << "\". Keys file wasn't found";
      r = false;
    }
  }

  return r;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::init(const boost::program_options::variables_map& vm)
{
  handle_command_line(vm);

  tools::password_container pwd_container;
  if (m_str_config_file.size())
  {
	  if (!init_config(m_str_config_file)) return false;

	  m_wallet_file = m_config.wallets_path_name;
	  pwd_container.password(m_config.password);

	  if (!m_generate_new.empty())
	  {
		  bool r = new_wallet(m_generate_new, pwd_container.password());
		  CHECK_AND_ASSERT_MES(r, false, "account creation failed");
	  }
	  else
	  {
		  bool r = open_wallet(m_wallet_file, pwd_container.password());
		  CHECK_AND_ASSERT_MES(r, false, "could not open account");
	  }
	  return true;
  }

  if (!m_daemon_address.empty() && !m_daemon_host.empty() && 0 != m_daemon_port)
  {
    fail_msg_writer() << "you can't specify daemon host or port several times";
    return false;
  }

  if(!m_generate_new.empty() && !m_wallet_file.empty())
  {
    fail_msg_writer() << "Specifying both --generate-new-wallet=\"wallet_name\" and --wallet-file=\"wallet_name\" doesn't make sense!";
    return false;
  }
  else if (m_generate_new.empty() && m_wallet_file.empty())
  {
    if(!ask_wallet_create_if_needed()) return false;
  }

  if (m_daemon_host.empty())
    m_daemon_host = "localhost";
  if (!m_daemon_port)
    m_daemon_port = RPC_DEFAULT_PORT;
  if (m_daemon_address.empty())
    m_daemon_address = std::string("http://") + m_daemon_host + ":" + std::to_string(m_daemon_port);

  if (command_line::has_arg(vm, arg_password))
  {
    pwd_container.password(command_line::get_arg(vm, arg_password));
  }
  else
  {
    bool r = pwd_container.read_password();
    if (!r)
    {
      fail_msg_writer() << "failed to read wallet password";
      return false;
    }
  }

  if (!m_generate_new.empty())
  {
    bool r = new_wallet(m_generate_new, pwd_container.password());
    CHECK_AND_ASSERT_MES(r, false, "account creation failed");
  }
  else
  {
    bool r = open_wallet(m_wallet_file, pwd_container.password());
    CHECK_AND_ASSERT_MES(r, false, "could not open account");
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::deinit()
{
  if (!m_wallet.get())
    return true;

  return close_wallet();
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::handle_command_line(const boost::program_options::variables_map& vm)
{
  m_wallet_file    = command_line::get_arg(vm, arg_wallet_file);
  m_generate_new   = command_line::get_arg(vm, arg_generate_new_wallet);
  m_daemon_address = command_line::get_arg(vm, arg_daemon_address);
  m_daemon_host    = command_line::get_arg(vm, arg_daemon_host);
  m_daemon_port    = command_line::get_arg(vm, arg_daemon_port);
  //m_str_config_file = command_line::get_arg(vm, arg_config_file);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::try_connect_to_daemon()
{
  if (!m_wallet->check_connection())
  {
    fail_msg_writer() << "wallet failed to connect to daemon (" << m_daemon_address << "). " <<
      "Daemon either is not started or passed wrong port. " <<
      "Please, make sure that daemon is running or restart the wallet with correct daemon address.";
    return false;
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(const string &wallet_file, const std::string& password)
{
  m_wallet_file = wallet_file;

  m_wallet.reset(new tools::wallet2());
  m_wallet->callback(this);
  try
  {
    m_wallet->generate(wallet_file, password);
    message_writer(epee::log_space::console_color_white, true) << "Generated new wallet: " << m_wallet->get_account().get_public_address_str() << std::endl << "view key: " << string_tools::pod_to_hex(m_wallet->get_account().get_keys().m_view_secret_key);
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << "failed to generate new wallet: " << e.what();
    return false;
  }

  m_wallet->init(m_daemon_address);

  success_msg_writer() <<
    "**********************************************************************\n" <<
    "Your wallet has been generated.\n" <<
    "To start synchronizing with the daemon use \"refresh\" command.\n" <<
    "Use \"help\" command to see the list of available commands.\n" <<
    "Always use \"exit\" command when closing simplewallet to save\n" <<
    "current session's state. Otherwise, you will possibly need to synchronize \n" <<
    "your wallet again. Your wallet key is NOT under risk anyway.\n" <<
    "**********************************************************************";
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::open_wallet(const string &wallet_file, const std::string& password)
{
  m_wallet_file = wallet_file;
  m_wallet.reset(new tools::wallet2());
  m_wallet->callback(this);

  try
  {
    m_wallet->load(m_wallet_file, password);
    message_writer(epee::log_space::console_color_white, true) << "Opened wallet: " << m_wallet->get_account().get_public_address_str();
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << "failed to load wallet: " << e.what();
    return false;
  }

  m_wallet->init(m_daemon_address);

  refresh(std::vector<std::string>());
  success_msg_writer() <<
    "**********************************************************************\n" <<
    "Use \"help\" command to see the list of available commands.\n" <<
    "**********************************************************************";
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::close_wallet()
{
  bool r = m_wallet->deinit();
  if (!r)
  {
    fail_msg_writer() << "failed to deinit wallet";
    return false;
  }

  try
  {
    m_wallet->store();
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
    return false;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::save(const std::vector<std::string> &args)
{
  try
  {
    m_wallet->store();
    success_msg_writer() << "Wallet data saved";
  }
  catch (const std::exception& e)
  {
    fail_msg_writer() << e.what();
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::start_mining(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  COMMAND_RPC_START_MINING::request req;
  req.miner_address = m_wallet->get_account().get_public_address_str();

  bool ok = true;
  size_t max_mining_threads_count = (std::max)(std::thread::hardware_concurrency(), static_cast<unsigned>(2));
  if (0 == args.size())
  {
    req.threads_count = 1;
  }
  else if (1 == args.size())
  {
    uint16_t num;
    ok = string_tools::get_xtype_from_string(num, args[0]);
    ok &= (1 <= num && num <= max_mining_threads_count);
    req.threads_count = num;
  }
  else
  {
    ok = false;
  }

  if (!ok)
  {
    fail_msg_writer() << "invalid arguments. Please use start_mining [<number_of_threads>], " <<
      "<number_of_threads> should be from 1 to " << max_mining_threads_count;
    return true;
  }

  COMMAND_RPC_START_MINING::response res;
  bool r = net_utils::invoke_http_json_remote_command2(m_daemon_address + "/start_mining", req, res, m_http_client);
  std::string err = interpret_rpc_response(r, res.status);
  if (err.empty())
    success_msg_writer() << "Mining started in daemon";
  else
    fail_msg_writer() << "mining has NOT been started: " << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::stop_mining(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  COMMAND_RPC_STOP_MINING::request req;
  COMMAND_RPC_STOP_MINING::response res;
  bool r = net_utils::invoke_http_json_remote_command2(m_daemon_address + "/stop_mining", req, res, m_http_client);
  std::string err = interpret_rpc_response(r, res.status);
  if (err.empty())
    success_msg_writer() << "Mining stopped in daemon";
  else
    fail_msg_writer() << "mining has NOT been stopped: " << err;
  return true;
}

//----------------------------------------------------------------------------------------------------
void simple_wallet::on_new_block(uint64_t height, const currency::block& block)
{
  m_refresh_progress_reporter.update(height, false);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_money_received(uint64_t height, const currency::transaction& tx, size_t out_index)
{
//  message_writer(epee::log_space::console_color_green, false) <<
//    "Height " << height <<
//    ", transaction " << get_transaction_hash(tx) <<
//    ", received " << print_money(tx.vout[out_index].amount);
  m_refresh_progress_reporter.update(height, true);
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::on_money_spent(uint64_t height, const currency::transaction& in_tx, size_t out_index, const currency::transaction& spend_tx)
{
//  message_writer(epee::log_space::console_color_magenta, false) <<
//    "Height " << height <<
//    ", transaction " << get_transaction_hash(spend_tx) <<
//    ", spent " << print_money(in_tx.vout[out_index].amount);
  m_refresh_progress_reporter.update(height, true);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::refresh(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  message_writer() << "Starting refresh...";
  size_t fetched_blocks = 0;
  bool ok = false;
  std::ostringstream ss;
  try
  {
    m_wallet->refresh(fetched_blocks);
    ok = true;
    // Clear line "Height xxx of xxx"
    std::cout << "\r                                                                \r";
    success_msg_writer(true) << "Refresh done, blocks received: " << fetched_blocks;
    show_balance();
  }
  catch (const tools::error::daemon_busy&)
  {
    ss << "daemon is busy. Please try later";
  }
  catch (const tools::error::no_connection_to_daemon&)
  {
    ss << "no connection to daemon. Please, make sure daemon is running";
  }
  catch (const tools::error::wallet_rpc_error& e)
  {
    LOG_ERROR("Unknown RPC error: " << e.to_string());
    ss << "RPC error \"" << e.what() << '"';
  }
  catch (const tools::error::refresh_error& e)
  {
    LOG_ERROR("refresh error: " << e.to_string());
    ss << e.what();
  }
  catch (const tools::error::wallet_internal_error& e)
  {
    LOG_ERROR("internal error: " << e.to_string());
    ss << "internal error: " << e.what();
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("unexpected error: " << e.what());
    ss << "unexpected error: " << e.what();
  }
  catch (...)
  {
    LOG_ERROR("Unknown error");
    ss << "unknown error";
  }

  if (!ok)
  {
    fail_msg_writer() << "refresh failed: " << ss.str() << ". Blocks received: " << fetched_blocks;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_balance(const std::vector<std::string>& args/* = std::vector<std::string>()*/)
{
  success_msg_writer() << "balance: " << print_money(m_wallet->balance()) << ", unlocked balance: " << print_money(m_wallet->unlocked_balance());
  return true;
}
//----------------------------------------------------------------------------------------------------
bool print_wti(const tools::wallet_rpc::wallet_transfer_info& wti)
{
  epee::log_space::console_colors cl;
  if (wti.is_income)
    cl = epee::log_space::console_color_green;
  else
    cl = epee::log_space::console_color_magenta;

  std::string payment_id_placeholder;
  if (wti.payment_id.size())
    payment_id_placeholder = std::string("(payment_id:") + wti.payment_id + ")";

  message_writer(cl) << epee::misc_utils::get_time_str_v2(wti.timestamp) << " "
    << (wti.is_income ? "Received " : "Sent    ")
    << print_money(wti.amount) << "(fee:" << print_money(wti.fee) << ")  "
    << (wti.recipient_alias.size() ? wti.recipient_alias : wti.recipient)
    << " " << wti.tx_hash << payment_id_placeholder;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::list_recent_transfers(const std::vector<std::string>& args)
{
  std::vector<tools::wallet_rpc::wallet_transfer_info> unconfirmed;
  std::vector<tools::wallet_rpc::wallet_transfer_info> recent;
  m_wallet->get_recent_transfers_history(recent, 0, 1000);
  m_wallet->get_unconfirmed_transfers(unconfirmed);
  //workaround for missed fee
  
  success_msg_writer() << "Unconfirmed transfers: ";
  for (auto & wti : unconfirmed)
  {
    if (!wti.fee)
      wti.fee = currency::get_tx_fee(wti.tx);
    print_wti(wti);
  }
  success_msg_writer() << "Recent transfers: ";
  for (auto & wti : recent)
  {
    if (!wti.fee)
      wti.fee = currency::get_tx_fee(wti.tx);
    print_wti(wti);
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_incoming_transfers(const std::vector<std::string>& args)
{
  bool filter = false;
  bool available = false;
  if (!args.empty())
  {
    if (args[0] == "available")
    {
      filter = true;
      available = true;
    }
    else if (args[0] == "unavailable")
    {
      filter = true;
      available = false;
    }
  }

  tools::wallet2::transfer_container transfers;
  m_wallet->get_transfers(transfers);

  bool transfers_found = false;
  for (const auto& td : transfers)
  {
    if (!filter || available != td.m_spent)
    {
      if (!transfers_found)
      {
        message_writer() << "        amount       \tspent\tglobal index\t                              tx id";
        transfers_found = true;
      }
      message_writer(td.m_spent ? epee::log_space::console_color_magenta : epee::log_space::console_color_green, false) <<
        std::setw(21) << print_money(td.amount()) << '\t' <<
        std::setw(3) << (td.m_spent ? 'T' : 'F') << "  \t" <<
        std::setw(12) << td.m_global_output_index << '\t' <<
        get_transaction_hash(td.m_tx) << "[" << td.m_block_height << "]";
    }
  }

  if (!transfers_found)
  {
    if (!filter)
    {
      success_msg_writer() << "No incoming transfers";
    }
    else if (available)
    {
      success_msg_writer() << "No incoming available transfers";
    }
    else
    {
      success_msg_writer() << "No incoming unavailable transfers";
    }
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_payments(const std::vector<std::string> &args)
{
  if(args.empty())
  {
    fail_msg_writer() << "expected at least one payment_id";
    return true;
  }

  message_writer() << "                            payment                             \t" <<
    "                          transaction                           \t" <<
    "  height\t       amount        \tunlock time";

  bool payments_found = false;
  for(std::string arg : args)
  {
    crypto::hash payment_id;
    if(parse_payment_id_from_hex_str(arg, payment_id))
    {
      std::list<tools::wallet2::payment_details> payments;
      m_wallet->get_payments(payment_id, payments);
      if(payments.empty())
      {
        success_msg_writer() << "No payments with id " << payment_id;
        continue;
      }

      for (const tools::wallet2::payment_details& pd : payments)
      {
        if(!payments_found)
        {
          payments_found = true;
        }
        success_msg_writer(true) <<
          payment_id << '\t' <<
          pd.m_tx_hash << '\t' <<
          std::setw(8)  << pd.m_block_height << '\t' <<
          std::setw(21) << print_money(pd.m_amount) << '\t' <<
          pd.m_unlock_time;
      }
    }
    else
    {
      fail_msg_writer() << "payment id has invalid format: \"" << arg << "\", expected 64-character string";
    }
  }

  return true;
}
//------------------------------------------------------------------------------------------------------------------
uint64_t simple_wallet::get_daemon_blockchain_height(std::string& err)
{
  COMMAND_RPC_GET_HEIGHT::request req;
  COMMAND_RPC_GET_HEIGHT::response res = boost::value_initialized<COMMAND_RPC_GET_HEIGHT::response>();
  bool r = net_utils::invoke_http_json_remote_command2(m_daemon_address + "/getheight", req, res, m_http_client);
  err = interpret_rpc_response(r, res.status);
  return res.height;
}
//------------------------------------------------------------------------------------------------------------------
//return false:  alias not found
//return true:   unkown or exists
bool simple_wallet::get_daemon_alias(const std::string& alias,std::string& err)
{
  epee::net_utils::http::http_simple_client http_client;

  currency::COMMAND_RPC_GET_ALIAS_DETAILS::request req = AUTO_VAL_INIT(req);
  req.alias = alias;
  currency::COMMAND_RPC_GET_ALIAS_DETAILS::response res = AUTO_VAL_INIT(res);
  bool r = epee::net_utils::invoke_http_json_rpc(m_daemon_address + "/json_rpc", "get_alias_details", req, res, http_client);
  err = res.status;

  if(res.status == "Alias not found")
	  return false;
  else
	  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_blockchain_height(const std::vector<std::string>& args)
{
  if (!try_connect_to_daemon())
    return true;

  std::string err;
  uint64_t bc_height = get_daemon_blockchain_height(err);
  if (err.empty())
    success_msg_writer() << bc_height;
  else
    fail_msg_writer() << "failed to get blockchain height: " << err;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::send_tx(const std::vector<std::string> &args_,std::vector<uint8_t> &extra,uint64_t fee)
{
  if (!try_connect_to_daemon())
    return true;

  std::vector<std::string> local_args = args_;
  if(local_args.size() < 3)
  {
    fail_msg_writer() << "wrong number of arguments, expected at least 3, got " << local_args.size();
    return true;
  }

  size_t fake_outs_count;
  if(!string_tools::get_xtype_from_string(fake_outs_count, local_args[0]))
  {
    fail_msg_writer() << "mixin_count should be non-negative integer, got " << local_args[0];
    return true;
  }
  local_args.erase(local_args.begin());

  if (1 == local_args.size() % 2)
  {
    std::string payment_id_str = local_args.back();
    local_args.pop_back();

    crypto::hash payment_id;
    bool r = parse_payment_id_from_hex_str(payment_id_str, payment_id);
    if(r)
    {
      r = set_payment_id_to_tx_extra(extra, payment_id);
    }

    if(!r)
    {
      fail_msg_writer() << "payment id has invalid format: \"" << payment_id_str << "\", expected 64-character string";
      return true;
    }
  }

  vector<currency::tx_destination_entry> dsts;
  for (size_t i = 0; i < local_args.size(); i += 2) 
  {
    currency::tx_destination_entry de;
    if(!m_wallet->get_transfer_address(local_args[i], de.addr))
    {
      fail_msg_writer() << "wrong address: " << local_args[i];
      return true;
    }

    if (local_args.size() <= i + 1)
    {
      fail_msg_writer() << "amount for the last address " << local_args[i] << " is not specified";
      return true;
    }

    bool ok = currency::parse_amount(de.amount, local_args[i + 1]);
	if(!ok ||  DEFAULT_FEE > de.amount)
    {
      fail_msg_writer() << "amount is wrong: " << local_args[i] << ' ' << local_args[i + 1] <<
        ", expected number from " <<  DEFAULT_FEE/COIN <<" to " << print_money(std::numeric_limits<uint64_t>::max()/COIN);
      return true;
    }

    dsts.push_back(de);
  }

  try
  {
    currency::transaction tx;
    m_wallet->transfer(dsts, fake_outs_count, 0, fee, extra, tx);
    success_msg_writer(true) << "Money successfully sent, transaction " << get_transaction_hash(tx) << ", " << get_object_blobsize(tx) << " bytes";
  }
  catch (const tools::error::daemon_busy&)
  {
    fail_msg_writer() << "daemon is busy. Please try later";
  }
  catch (const tools::error::no_connection_to_daemon&)
  {
    fail_msg_writer() << "no connection to daemon. Please, make sure daemon is running.";
  }
  catch (const tools::error::wallet_rpc_error& e)
  {
    LOG_ERROR("Unknown RPC error: " << e.to_string());
    fail_msg_writer() << "RPC error \"" << e.what() << '"';
  }
  catch (const tools::error::get_random_outs_error&)
  {
    fail_msg_writer() << "failed to get random outputs to mix";
  }
  catch (const tools::error::not_enough_money& e)
  {
    fail_msg_writer() << "not enough money to transfer, available only " << print_money(e.available()) <<
      ", transaction amount " << print_money(e.tx_amount() + e.fee()) << " = " << print_money(e.tx_amount()) <<
      " + " << print_money(e.fee()) << " (fee)";
  }
  catch (const tools::error::not_enough_outs_to_mix& e)
  {
    auto writer = fail_msg_writer();
    writer << "not enough outputs for specified mixin_count = " << e.mixin_count() << ":";
    for (const currency::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& outs_for_amount : e.scanty_outs())
    {
      writer << "\noutput amount = " << print_money(outs_for_amount.amount) << ", fount outputs to mix = " << outs_for_amount.outs.size();
    }
  }
  catch (const tools::error::tx_not_constructed&)
  {
    fail_msg_writer() << "transaction was not constructed";
  }
  catch (const tools::error::tx_rejected& e)
  {
    fail_msg_writer() << "transaction " << get_transaction_hash(e.tx()) << " was rejected by daemon with status \"" << e.status() << '"';
  }
  catch (const tools::error::tx_sum_overflow& e)
  {
    fail_msg_writer() << e.what();
  }
  catch (const tools::error::tx_too_big& e)
  {
    currency::transaction tx = e.tx();
    fail_msg_writer() << "transaction " << get_transaction_hash(e.tx()) << " is too big. Transaction size: " <<
      get_object_blobsize(e.tx()) << " bytes, transaction size limit: " << e.tx_size_limit() << " bytes. Try to separate this payment into few smaller transfers.";
  }
  catch (const tools::error::zero_destination&)
  {
    fail_msg_writer() << "one of destinations is zero";
  }
  catch (const tools::error::transfer_error& e)
  {
    LOG_ERROR("unknown transfer error: " << e.to_string());
    fail_msg_writer() << "unknown transfer error: " << e.what();
  }
  catch (const tools::error::wallet_internal_error& e)
  {
    LOG_ERROR("internal error: " << e.to_string());
    fail_msg_writer() << "internal error: " << e.what();
  }
  catch (const std::exception& e)
  {
    LOG_ERROR("unexpected error: " << e.what());
    fail_msg_writer() << "unexpected error: " << e.what();
  }
  catch (...)
  {
    LOG_ERROR("Unknown error");
    fail_msg_writer() << "unknown error";
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::transfer(const std::vector<std::string> &args_)
{
  std::vector<uint8_t> extra;  
  return send_tx(args_,extra,DEFAULT_FEE);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::make_alias(const std::vector<std::string> &args)
{
	if(args.size() < 1 || args.size() > 3)
	{
		std::cout << "usage: make_alias <alias>  [comment] [viewkey]" << std::endl;
		return true;
	}
	std::string str;
	if (m_wallet->get_blockchain_current_height() < CURRENCY_BLOCK_GRANTED_FULL_REWARD_ZONE_ENLARGE_STARTING_BLOCK)
	{
		str = "Alias name can't be make untill the " + boost::to_string(CURRENCY_BLOCK_GRANTED_FULL_REWARD_ZONE_ENLARGE_STARTING_BLOCK);
		str += "th block reached.";
		std::cout << str;
		return true;
	}

	std::string alias = args[0];
	str.resize(alias.size());
	std::transform(alias.begin(), alias.end(), str.begin(), (int(*)(int))std::tolower);

	if (!currency::validate_alias_name(str))
	{
		std::cout << "Wrong alias name" << std::endl;
		return true;
	}

	std::string err;
	if (get_daemon_alias(str, err))
	{
		if(err == CORE_RPC_STATUS_OK)
			std::cout << "Alias already exists" << std::endl;
		else if(err == "")
			std::cout << "No connection to daemon" << std::endl;
		else
			std::cout << err << std::endl;
		return true;
	}
	
	currency::alias_info ai = AUTO_VAL_INIT(ai);
	ai.m_alias = str;

	if (!currency::get_account_address_from_str(ai.m_address, m_wallet->get_account().get_public_address_str()))
	{
		std::cout << "target account address has wrong format" << std::endl;
		return true;
	}

	if(args.size() >= 2)
	{
		if(args[1].size() > 64)
		{
			std::cout << "comment too long, must be less 64 chars." << std::endl;
			return true;
		}
		ai.m_text_comment = args[1];
	}	
	
	if(args.size() == 3)
	{
		str = args[2];		
		if(str.size() != 64)
		{
			std::cout << "viewkey has wrong length, must be 64 chars  or empty." << std::endl;
			return true;
		}
		std::string bin_str;
		epee::string_tools::parse_hexstr_to_binbuff(str, bin_str);
		ai.m_view_key = *reinterpret_cast<const crypto::secret_key*>(bin_str.c_str());
	}

	std::vector<std::string> myargs;
	myargs.push_back("0");
	myargs.push_back(CURRENCY_DONATIONS_ADDRESS);
	myargs.push_back("1");

    std::vector<uint8_t> extra;
	std::string buff;
    bool r = make_tx_extra_alias_entry(buff, ai);
    if(!r) return false;
    extra.resize(buff.size());
    memcpy(&extra[0], buff.data(), buff.size());

	return send_tx(myargs,extra,MAKE_ALIAS_MINIMUM_FEE);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::run()
{
  std::string addr_start = m_wallet->get_account().get_public_address_str().substr(0, 6);
  return m_cmd_binder.run_handling("[" CURRENCY_NAME_BASE " wallet " + addr_start + "]: ", "");
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::stop()
{
  m_cmd_binder.stop_handling();
  m_wallet->stop();
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_address(const std::vector<std::string> &args/* = std::vector<std::string>()*/)
{
  success_msg_writer() << m_wallet->get_account().get_public_address_str();
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::process_command(const std::vector<std::string> &args)
{
  return m_cmd_binder.process_command_vec(args);
}
//----------------------------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
#ifdef WIN32
  _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif

  //TRY_ENTRY();

  string_tools::set_module_name_and_folder(argv[0]);

  po::options_description desc_general("General options");
  command_line::add_arg(desc_general, command_line::arg_help);
  command_line::add_arg(desc_general, command_line::arg_version);

  po::options_description desc_params("Wallet options");
  command_line::add_arg(desc_params, arg_wallet_file);
  command_line::add_arg(desc_params, arg_generate_new_wallet);
  command_line::add_arg(desc_params, arg_password);
  command_line::add_arg(desc_params, arg_daemon_address);
  command_line::add_arg(desc_params, arg_daemon_host);
  command_line::add_arg(desc_params, arg_daemon_port);
  command_line::add_arg(desc_params, arg_command);
  command_line::add_arg(desc_params, arg_log_level);
  tools::wallet_rpc_server::init_options(desc_params);

  po::positional_options_description positional_options;
  positional_options.add(arg_command.name, -1);

  po::options_description desc_all;
  desc_all.add(desc_general).add(desc_params);
  currency::simple_wallet w;
  po::variables_map vm;
  bool r = command_line::handle_error_helper(desc_all, [&]()
  {
    po::store(command_line::parse_command_line(argc, argv, desc_general, true), vm);

    if (command_line::get_arg(vm, command_line::arg_help))
    {
	  success_msg_writer() << CURRENCY_NAME << " wallet v" << PROJECT_VERSION_LONG;
      success_msg_writer() << "Usage: simplewallet [--wallet-file=<file>|--generate-new-wallet=<file>] [--daemon-address=<host>:<port>] [<COMMAND>]";
      success_msg_writer() << desc_all << '\n' << w.get_commands_str();
      return false;
    }
    else if (command_line::get_arg(vm, command_line::arg_version))
    {
      success_msg_writer() << CURRENCY_NAME << " wallet v" << PROJECT_VERSION_LONG;
      return false;
    }

    auto parser = po::command_line_parser(argc, argv).options(desc_params).positional(positional_options);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });
  if (!r)
    return 1;

  //set up logging options
  log_space::get_set_log_detalisation_level(true, LOG_LEVEL_2);
  //log_space::log_singletone::add_logger(LOGGER_CONSOLE, NULL, NULL, LOG_LEVEL_0);
  log_space::log_singletone::add_logger(LOGGER_FILE,
    log_space::log_singletone::get_default_log_file().c_str(),
    log_space::log_singletone::get_default_log_folder().c_str(), LOG_LEVEL_4);

  message_writer(epee::log_space::console_color_white, true) << CURRENCY_NAME << " wallet v" << PROJECT_VERSION_LONG;

  if(command_line::has_arg(vm, arg_log_level))
  {
    LOG_PRINT_L0("Setting log level = " << command_line::get_arg(vm, arg_log_level));
    log_space::get_set_log_detalisation_level(true, command_line::get_arg(vm, arg_log_level));
  }
  
  if(command_line::has_arg(vm, tools::wallet_rpc_server::arg_rpc_bind_port))
  {
    log_space::log_singletone::add_logger(LOGGER_CONSOLE, NULL, NULL, LOG_LEVEL_2);
    //runs wallet with rpc interface 
    if(!command_line::has_arg(vm, arg_wallet_file) )
    {
      LOG_ERROR("Wallet file not set.");
      return 1;
    }
    if(!command_line::has_arg(vm, arg_daemon_address) )
    {
      LOG_ERROR("Daemon address not set.");
      return 1;
    }
    if(!command_line::has_arg(vm, arg_password) )
    {
      LOG_ERROR("Wallet password not set.");
      return 1;
    }

    std::string wallet_file     = command_line::get_arg(vm, arg_wallet_file);    
    std::string wallet_password = command_line::get_arg(vm, arg_password);
    std::string daemon_address  = command_line::get_arg(vm, arg_daemon_address);
    std::string daemon_host = command_line::get_arg(vm, arg_daemon_host);
    int daemon_port = command_line::get_arg(vm, arg_daemon_port);
    if (daemon_host.empty())
      daemon_host = "localhost";
    if (!daemon_port)
      daemon_port = RPC_DEFAULT_PORT;
    if (daemon_address.empty())
      daemon_address = std::string("http://") + daemon_host + ":" + std::to_string(daemon_port);

    tools::wallet2 wal;
    try
    {
      LOG_PRINT_L0("Loading wallet...");
      wal.load(wallet_file, wallet_password);
      wal.init(daemon_address);
      wal.refresh();
      LOG_PRINT_GREEN("Loaded ok", LOG_LEVEL_0);
    }
    catch (const std::exception& e)
    {
      LOG_ERROR("Wallet initialize failed: " << e.what());
      return 1;
    }
    tools::wallet_rpc_server wrpc(wal);
    bool r = wrpc.init(vm);
    CHECK_AND_ASSERT_MES(r, 1, "Failed to initialize wallet rpc server");


    tools::signal_handler::install([&wrpc, &wal] {
      wrpc.send_stop_signal();
      wal.store();
    });
	LOG_PRINT_L0("Starting wallet rpc server");
    wrpc.run();
    LOG_PRINT_L0("Stopped wallet rpc server");
    try
    {
      LOG_PRINT_L0("Storing wallet...");
      wal.store();
      LOG_PRINT_GREEN("Stored ok", LOG_LEVEL_0);
    }
    catch (const std::exception& e)
    {
      LOG_ERROR("Failed to store wallet: " << e.what());
      return 1;
    }
  }else
  {
    //runs wallet with console interface 
    r = w.init(vm);
    CHECK_AND_ASSERT_MES(r, 1, "Failed to initialize wallet");

    std::vector<std::string> command = command_line::get_arg(vm, arg_command);
    if (!command.empty())
    {
      w.process_command(command);
      w.stop();
      w.deinit();
    }
    else
    {
      tools::signal_handler::install([&w] {
        w.stop();
      });
      w.run();

      w.deinit();
    }
  }
  return 1;
  //CATCH_ENTRY_L0("main", 1);
}
