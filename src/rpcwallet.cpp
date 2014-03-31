// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "wallet.h"
#include "walletdb.h"
#include "bitcoinrpc.h"
#include "init.h"
#include "base58.h"
#include "alias.h"
#include "offer.h"
#include "txdb.h"
#include "script.h"

#include <boost/xpressive/xpressive_dynamic.hpp>

using namespace std;
using namespace boost;
using namespace boost::assign;
using namespace json_spirit;

int64 nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

extern CNameDB *pnamedb;

template<typename T> void ConvertTo(Value& value, bool fAllowNull=false);

inline bool AddressToHash160(const char* psz, uint160& hash160Ret);


std::string HelpRequiringPassphrase()
{
    return pwalletMain && pwalletMain->IsCrypted()
        ? "\nrequires wallet passphrase to be set with walletpassphrase first"
        : "";
}

void EnsureWalletIsUnlocked()
{
    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
}

void WalletTxToJSON(const CWalletTx& wtx, Object& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.push_back(Pair("confirmations", confirms));
    if (wtx.IsCoinBase())
        entry.push_back(Pair("generated", true));
    if (confirms)
    {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        entry.push_back(Pair("blocktime", (boost::int64_t)(mapBlockIndex[wtx.hashBlock]->nTime)));
    }
    entry.push_back(Pair("txid", wtx.GetHash().GetHex()));
    entry.push_back(Pair("time", (boost::int64_t)wtx.GetTxTime()));
    entry.push_back(Pair("data", wtx.GetBase64Data()));
    entry.push_back(Pair("timereceived", (boost::int64_t)wtx.nTimeReceived));
    BOOST_FOREACH(const PAIRTYPE(string,string)& item, wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

string AccountFromValue(const Value& value)
{
    string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

Value getinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getinfo\n"
            "Returns an object containing various state info.");

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    Object obj;
    obj.push_back(Pair("version",       (int)CLIENT_VERSION));
    obj.push_back(Pair("protocolversion",(int)PROTOCOL_VERSION));
    if (pwalletMain) {
        obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
        obj.push_back(Pair("balance",       ValueFromAmount(pwalletMain->GetBalance())));
    }
    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("timeoffset",    (boost::int64_t)GetTimeOffset()));
    obj.push_back(Pair("connections",   (int)vNodes.size()));
    obj.push_back(Pair("proxy",         (proxy.first.IsValid() ? proxy.first.ToStringIPPort() : string())));
    obj.push_back(Pair("difficulty",    (double)GetDifficulty()));
    obj.push_back(Pair("testnet",       fTestNet));
    if (pwalletMain) {
        obj.push_back(Pair("keypoololdest", (boost::int64_t)pwalletMain->GetOldestKeyPoolTime()));
        obj.push_back(Pair("keypoolsize",   (int)pwalletMain->GetKeyPoolSize()));
    }
    obj.push_back(Pair("paytxfee",      ValueFromAmount(nTransactionFee)));
    obj.push_back(Pair("mininput",      ValueFromAmount(nMinimumInputValue)));
    if (pwalletMain && pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", (boost::int64_t)nWalletUnlockTime));
    obj.push_back(Pair("errors",        GetWarnings("statusbar")));
    return obj;
}



Value getnewaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getnewaddress [account]\n"
            "Returns a new SysCoin address for receiving payments.  "
            "If [account] is specified (recommended), it is added to the address book "
            "so payments received with the address will be credited to [account].");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (params.size() > 0)
        strAccount = AccountFromValue(params[0]);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();

    pwalletMain->SetAddressBookName(keyID, strAccount);

    return CBitcoinAddress(keyID).ToString();
}


CBitcoinAddress GetAccountAddress(string strAccount, bool bForceNew=false)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    bool bKeyUsed = false;

    // Check if the current key has been used
    if (account.vchPubKey.IsValid())
    {
        CScript scriptPubKey;
        scriptPubKey.SetDestination(account.vchPubKey.GetID());
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end() && account.vchPubKey.IsValid();
             ++it)
        {
            const CWalletTx& wtx = (*it).second;
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
                if (txout.scriptPubKey == scriptPubKey)
                    bKeyUsed = true;
        }
    }

    // Generate a new key
    if (!account.vchPubKey.IsValid() || bForceNew || bKeyUsed)
    {
        if (!pwalletMain->GetKeyFromPool(account.vchPubKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

        pwalletMain->SetAddressBookName(account.vchPubKey.GetID(), strAccount);
        walletdb.WriteAccount(strAccount, account);
    }

    return CBitcoinAddress(account.vchPubKey.GetID());
}

Value getaccountaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccountaddress <account>\n"
            "Returns the current SysCoin address for receiving payments to this account.");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount = AccountFromValue(params[0]);

    Value ret;

    ret = GetAccountAddress(strAccount).ToString();

    return ret;
}



Value setaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setaccount <syscoinaddress> <account>\n"
            "Sets the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid SysCoin address");


    string strAccount;
    if (params.size() > 1)
        strAccount = AccountFromValue(params[1]);

    // Detect when changing the account of an address that is the 'unused current key' of another account:
    if (pwalletMain->mapAddressBook.count(address.Get()))
    {
        string strOldAccount = pwalletMain->mapAddressBook[address.Get()];
        if (address == GetAccountAddress(strOldAccount))
            GetAccountAddress(strOldAccount, true);
    }

    pwalletMain->SetAddressBookName(address.Get(), strAccount);

    return Value::null;
}


Value getaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccount <syscoinaddress>\n"
            "Returns the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid SysCoin address");

    string strAccount;
    map<CTxDestination, string>::iterator mi = pwalletMain->mapAddressBook.find(address.Get());
    if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.empty())
        strAccount = (*mi).second;
    return strAccount;
}


Value getaddressesbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressesbyaccount <account>\n"
            "Returns the list of addresses for the given account.");

    string strAccount = AccountFromValue(params[0]);

    // Find all addresses that have the given account
    Array ret;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            ret.push_back(address.ToString());
    }
    return ret;
}


Value setmininput(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error(
            "setmininput <amount>\n"
            "<amount> is a real and is rounded to the nearest 0.00000001");

    // Amount
    int64 nAmount = 0;
    if (params[0].get_real() != 0.0)
        nAmount = AmountFromValue(params[0]);        // rejects 0.0 amounts

    nMinimumInputValue = nAmount;
    return true;
}


Value sendtoaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
        throw runtime_error(
            "sendtoaddress <datacoinaddress> <amount> [comment] [comment-to] [data]\n"
            "<amount> is a real and is rounded to the nearest 0.00000001"
            "<data> is a base64 encoded data chunk"
            + HelpRequiringPassphrase());

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid SysCoin address");

    // Amount
    int64 nAmount = AmountFromValue(params[1]);
    if (nAmount < MIN_TXOUT_AMOUNT)
        throw JSONRPCError(-101, "Send amount too small");

    // Wallet comments
    CWalletTx wtx;
    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["to"]      = params[3].get_str();

    // Transaction data
    std::string txdata;
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty()) {
        txdata = params[4].get_str();
        if (txdata.length() > MAX_TX_DATA_SIZE)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "data chunk is too long. split it the payload to several transactions.");
    }

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx, false, txdata);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value senddata(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "senddata [data]\n"
            "<data> is a base64 encoded data chunk"
            + HelpRequiringPassphrase());

    CWalletTx wtx;

    // Transaction data
    std::string txdata;
    if (params.size() > 0 && params[0].type() != null_type && !params[0].get_str().empty()) {
        txdata = params[0].get_str();
        if (txdata.length() > MAX_TX_DATA_SIZE)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "data chunk is too long. split it the payload to several transactions.");
    }

    string strError = pwalletMain->SendData(wtx, false, txdata);

    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value listaddressgroupings(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listaddressgroupings\n"
            "Lists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions");

    Array jsonGroupings;
    map<CTxDestination, int64> balances = pwalletMain->GetAddressBalances();
    BOOST_FOREACH(set<CTxDestination> grouping, pwalletMain->GetAddressGroupings())
    {
        Array jsonGrouping;
        BOOST_FOREACH(CTxDestination address, grouping)
        {
            Array addressInfo;
            addressInfo.push_back(CBitcoinAddress(address).ToString());
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                LOCK(pwalletMain->cs_wallet);
                if (pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get()) != pwalletMain->mapAddressBook.end())
                    addressInfo.push_back(pwalletMain->mapAddressBook.find(CBitcoinAddress(address).Get())->second);
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

Value signmessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "signmessage <syscoinaddress> <message>\n"
            "Sign a message with the private key of an address");

    EnsureWalletIsUnlocked();

    string strAddress = params[0].get_str();
    string strMessage = params[1].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

Value verifymessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "verifymessage <syscoinaddress> <signature> <message>\n"
            "Verify a signed message");

    string strAddress  = params[0].get_str();
    string strSign     = params[1].get_str();
    string strMessage  = params[2].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;

    return (pubkey.GetID() == keyID);
}


Value getreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaddress <syscoinaddress> [minconf=1]\n"
            "Returns the total amount received by <syscoinaddress> in transactions with at least [minconf] confirmations.");

    // Bitcoin address
    CBitcoinAddress address = CBitcoinAddress(params[0].get_str());
    CScript scriptPubKey;
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid SysCoin address");
    scriptPubKey.SetDestination(address.Get());
    if (!IsMine(*pwalletMain,scriptPubKey))
        return (double)0.0;

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Tally
    int64 nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


void GetAccountAddresses(string strAccount, set<CTxDestination>& setAddress)
{
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& item, pwalletMain->mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            setAddress.insert(address);
    }
}

Value getreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaccount <account> [minconf=1]\n"
            "Returns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.");

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Get the set of pub keys assigned to account
    string strAccount = AccountFromValue(params[0]);
    set<CTxDestination> setAddress;
    GetAccountAddresses(strAccount, setAddress);

    // Tally
    int64 nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwalletMain, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
        }
    }

    return (double)nAmount / (double)COIN;
}


int64 GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth)
{
    int64 nBalance = 0;

    // Tally wallet transactions
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (!wtx.IsFinal())
            continue;

        int64 nReceived, nSent, nFee;
        wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee);

        if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth)
            nBalance += nReceived;
        nBalance -= nSent + nFee;
    }

    // Tally internal accounting entries
    nBalance += walletdb.GetAccountCreditDebit(strAccount);

    return nBalance;
}

int64 GetAccountBalance(const string& strAccount, int nMinDepth)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);
    return GetAccountBalance(walletdb, strAccount, nMinDepth);
}


Value getbalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getbalance [account] [minconf=1]\n"
            "If [account] is not specified, returns the server's total available balance.\n"
            "If [account] is specified, returns the balance in the account.");

    if (params.size() == 0)
        return  ValueFromAmount(pwalletMain->GetBalance());

    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    if (params[0].get_str() == "*") {
        // Calculate total balance a different way from GetBalance()
        // (GetBalance() sums up all unspent TxOuts)
        // getbalance and getbalance '*' 0 should return the same number
        int64 nBalance = 0;
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx& wtx = (*it).second;
            if (!wtx.IsConfirmed())
                continue;

            int64 allFee;
            string strSentAccount;
            list<pair<CTxDestination, int64> > listReceived;
            list<pair<CTxDestination, int64> > listSent;
            bool fNameTx;
            wtx.GetAmounts(listReceived, listSent, allFee, strSentAccount, fNameTx);
            if (wtx.GetDepthInMainChain() >= nMinDepth)
            {
                BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& r, listReceived)
                    nBalance += r.second;
            }
            BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& r, listSent)
                nBalance -= r.second;
            nBalance -= allFee;
        }
        return  ValueFromAmount(nBalance);
    }

    string strAccount = AccountFromValue(params[0]);

    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);

    return ValueFromAmount(nBalance);
}


Value movecmd(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 5)
        throw runtime_error(
            "move <fromaccount> <toaccount> <amount> [minconf=1] [comment]\n"
            "Move from one account in your wallet to another.");

    string strFrom = AccountFromValue(params[0]);
    string strTo = AccountFromValue(params[1]);
    int64 nAmount = AmountFromValue(params[2]);
    if (params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)params[3].get_int();
    string strComment;
    if (params.size() > 4)
        strComment = params[4].get_str();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    if (!walletdb.TxnBegin())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    int64 nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    walletdb.WriteAccountingEntry(debit);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    walletdb.WriteAccountingEntry(credit);

    if (!walletdb.TxnCommit())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    return true;
}


Value sendfrom(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 7)
        throw runtime_error(
            "sendfrom <fromaccount> <todatacoinaddress> <amount> [minconf=1] [comment] [comment-to] [data]\n"
            "<amount> is a real and is rounded to the nearest 0.00000001"
            "<data> is a base64 encoded data chunk"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    CBitcoinAddress address(params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid SysCoin address");
    int64 nAmount = AmountFromValue(params[2]);
    if (nAmount < MIN_TXOUT_AMOUNT)
        throw JSONRPCError(-101, "Send amount too small");
    int nMinDepth = 1;
    if (params.size() > 3)
        nMinDepth = params[3].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty())
        wtx.mapValue["comment"] = params[4].get_str();
    if (params.size() > 5 && params[5].type() != null_type && !params[5].get_str().empty())
        wtx.mapValue["to"]      = params[5].get_str();

    std::string txdata;
    if (params.size() > 6 && params[6].type() != null_type && !params[6].get_str().empty()) {
        txdata = params[6].get_str();
        if (txdata.length() > MAX_TX_DATA_SIZE)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "data chunk is too long. split it the payload to several transactions.");
    }

    EnsureWalletIsUnlocked();

    // Check funds
    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx, false, txdata);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}


Value sendmany(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
        throw runtime_error(
            "sendmany <fromaccount> {address:amount,...} [minconf=1] [comment] [data]\n"
            "amounts are double-precision floating point numbers"
            "<data> is a base64 encoded data chunk"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    Object sendTo = params[1].get_obj();
    int nMinDepth = 1;
    if (params.size() > 2)
        nMinDepth = params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["comment"] = params[3].get_str();

    std::string txdata;
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty()) {
       txdata = params[4].get_str();
        if (txdata.length() > MAX_TX_DATA_SIZE)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "data chunk is too long. split it the payload to several transactions.");
    }

    set<CBitcoinAddress> setAddress;
    vector<pair<CScript, int64> > vecSend;

    int64 totalAmount = 0;
    BOOST_FOREACH(const Pair& s, sendTo)
    {
        CBitcoinAddress address(s.name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Datacoin address: ")+s.name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+s.name_);
        setAddress.insert(address);

        CScript scriptPubKey;
        scriptPubKey.SetDestination(address.Get());
        int64 nAmount = AmountFromValue(s.value_);
        if (nAmount < MIN_TXOUT_AMOUNT)
            throw JSONRPCError(-101, "Send amount too small");
        totalAmount += nAmount;

        vecSend.push_back(make_pair(scriptPubKey, nAmount));
    }

    EnsureWalletIsUnlocked();

    // Check funds
    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwalletMain);
    int64 nFeeRequired = 0;
    string strFailReason;
    bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, strFailReason, NULL, txdata);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    if (!pwalletMain->CommitTransaction(wtx, keyChange))
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction commit failed");

    return wtx.GetHash().GetHex();
}

//
// Used by addmultisigaddress / createmultisig:
//
static CScript _createmultisig(const Array& params)
{
    int nRequired = params[0].get_int();
    const Array& keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1)
        throw runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw runtime_error(
            strprintf("not enough keys supplied "
                      "(got %"PRIszu" keys, but need at least %d to redeem)", keys.size(), nRequired));
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        const std::string& ks = keys[i].get_str();

        // Case 1: SysCoin address and we have full public key:
        CBitcoinAddress address(ks);
        if (pwalletMain && address.IsValid())
        {
            CKeyID keyID;
            if (!address.GetKeyID(keyID))
                throw runtime_error(
                    strprintf("%s does not refer to a key",ks.c_str()));
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(keyID, vchPubKey))
                throw runtime_error(
                    strprintf("no full public key for address %s",ks.c_str()));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
            pubkeys[i] = vchPubKey;
        }

        // Case 2: hex public key
        else if (IsHex(ks))
        {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
            pubkeys[i] = vchPubKey;
        }
        else
        {
            throw runtime_error(" Invalid public key: "+ks);
        }
    }
    CScript result;
    result.SetMultisig(nRequired, pubkeys);
    return result;
}

Value addmultisigaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
    {
        string msg = "addmultisigaddress <nrequired> <'[\"key\",\"key\"]'> [account]\n"
            "Add a nrequired-to-sign multisignature address to the wallet\"\n"
            "each key is a SysCoin address or hex-encoded public key\n"
            "If [account] is specified, assign address to [account].";
        throw runtime_error(msg);
    }

    string strAccount;
    if (params.size() > 2)
        strAccount = AccountFromValue(params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig(params);
    CScriptID innerID = inner.GetID();
    pwalletMain->AddCScript(inner);

    pwalletMain->SetAddressBookName(innerID, strAccount);
    return CBitcoinAddress(innerID).ToString();
}

Value createmultisig(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 2)
    {
        string msg = "createmultisig <nrequired> <'[\"key\",\"key\"]'>\n"
            "Creates a multi-signature address and returns a json object\n"
            "with keys:\n"
            "address : syscoin address\n"
            "redeemScript : hex-encoded redemption script";
        throw runtime_error(msg);
    }

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig(params);
    CScriptID innerID = inner.GetID();
    CBitcoinAddress address(innerID);

    Object result;
    result.push_back(Pair("address", address.ToString()));
    result.push_back(Pair("redeemScript", HexStr(inner.begin(), inner.end())));

    return result;
}


struct tallyitem
{
    int64 nAmount;
    int nConf;
    vector<uint256> txids;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
    }
};

Value ListReceived(const Array& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    // Tally
    map<CBitcoinAddress, tallyitem> mapTally;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;

        if (wtx.IsCoinBase() || !wtx.IsFinal())
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address) || !IsMine(*pwalletMain, address))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
        }
    }

    // Reply
    Array ret;
    map<string, tallyitem> mapAccountTally;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strAccount = item.second;
        map<CBitcoinAddress, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        int64 nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
        }

        if (fByAccounts)
        {
            tallyitem& item = mapAccountTally[strAccount];
            item.nAmount += nAmount;
            item.nConf = min(item.nConf, nConf);
        }
        else
        {
            Object obj;
            obj.push_back(Pair("address",       address.ToString()));
            obj.push_back(Pair("account",       strAccount));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            Array transactions;
            if (it != mapTally.end())
            {
                BOOST_FOREACH(const uint256& item, (*it).second.txids)
                {
                    transactions.push_back(item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts)
    {
        for (map<string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it)
        {
            int64 nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            Object obj;
            obj.push_back(Pair("account",       (*it).first));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

Value listreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listreceivedbyaddress [minconf=1] [includeempty=false]\n"
            "[minconf] is the minimum number of confirmations before payments are included.\n"
            "[includeempty] whether to include addresses that haven't received any payments.\n"
            "Returns an array of objects containing:\n"
            "  \"address\" : receiving address\n"
            "  \"account\" : the account of the receiving address\n"
            "  \"amount\" : total amount received by the address\n"
            "  \"confirmations\" : number of confirmations of the most recent transaction included\n"
            "  \"txids\" : list of transactions with outputs to the address\n");

    return ListReceived(params, false);
}

Value listreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listreceivedbyaccount [minconf=1] [includeempty=false]\n"
            "[minconf] is the minimum number of confirmations before payments are included.\n"
            "[includeempty] whether to include accounts that haven't received any payments.\n"
            "Returns an array of objects containing:\n"
            "  \"account\" : the account of the receiving addresses\n"
            "  \"amount\" : total amount received by addresses with this account\n"
            "  \"confirmations\" : number of confirmations of the most recent transaction included");

    return ListReceived(params, true);
}

void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, Array& ret) {
    int64 nFee;
    string strSentAccount;
    list<pair<CTxDestination, int64> > listReceived;
    list<pair<CTxDestination, int64> > listSent;
    bool fNameTx;
    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, fNameTx);

    bool fAllAccounts = (strAccount == string("*"));

    // Sent
    if ((!listSent.empty() || nFee != 0 || fNameTx) && (fAllAccounts || strAccount == strSentAccount)) {
        if (listSent.empty() || fNameTx) {
            // alias transaction, or some non-standard transaction with non-zero fee
            Object entry;
            entry.push_back(Pair("account", strSentAccount));
            string strAddress;
            if (fNameTx) {
                vector<vector<unsigned char> > vvchArgs;
                int op,nOut, nTxOut;
                bool good = DecodeNameTx(wtx, op, nOut, vvchArgs, -1);
                if(IsAliasOp(op)) {
                    nTxOut = IndexOfNameOutput(wtx);
                    ExtractAliasAddress(wtx.vout[nTxOut].scriptPubKey, strAddress);
                } else {
                    good = DecodeOfferTx(wtx, op, nOut, vvchArgs, -1);
                    if (!good || !IsOfferOp(op)) {
                    	JSONRPCError(RPC_WALLET_ERROR, "ListTransactions() : could not decode a syscoin tx");
                    	return;
                    }
                    nTxOut = IndexOfOfferOutput(wtx);
                    ExtractOfferAddress(wtx.vout[nTxOut].scriptPubKey, strAddress);
                }
            }
            entry.push_back(Pair("address", strAddress));
            entry.push_back(Pair("category", "send"));
            entry.push_back(Pair("amount", ValueFromAmount(0)));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
        else {
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& s, listSent) {
                Object entry;
                entry.push_back(Pair("account", strSentAccount));
                entry.push_back(Pair("address", CBitcoinAddress(s.first).ToString()));
                entry.push_back(Pair("category", "send"));
                entry.push_back(Pair("amount", ValueFromAmount(-s.second)));
                entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
        BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& r, listReceived) {
            string account;
            if (pwalletMain->mapAddressBook.count(r.first))
                account = pwalletMain->mapAddressBook[r.first];
            if (fAllAccounts || (account == strAccount)) {
                Object entry;
                entry.push_back(Pair("account", account));
                entry.push_back(Pair("address", CBitcoinAddress(r.first).ToString()));
                if (wtx.IsCoinBase()) {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                }
                else
                    entry.push_back(Pair("category", "receive"));
                entry.push_back(Pair("amount", ValueFromAmount(r.second)));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, Array& ret)
{
    bool fAllAccounts = (strAccount == string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        Object entry;
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", (boost::int64_t)acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

Value dumpdata(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "dumpdata [hash]\n"
            "<data> is the base64 encoded tx hash of the data"
            + HelpRequiringPassphrase());

    uint256 hash;
    hash.SetHex(params[0].get_str());

    Object entry;
    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");

    const CWalletTx& wtx = pwalletMain->mapWallet[hash];

    return wtx.GetBase64Data();
}

Value setdata(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "setdata [data]\n"
            "<data> is a base64 encoded data chunk"
            + HelpRequiringPassphrase());

    CWalletTx wtx;

    // Transaction data
    std::string txdata;
    if (params.size() > 0 && params[0].type() != null_type && !params[0].get_str().empty()) {
        txdata = params[0].get_str();
        if (txdata.length() > MAX_TX_DATA_SIZE)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Data chunk is too long.  Split the payload to several transactions.");
    }

    string strError = pwalletMain->SendData(wtx, false, txdata);

    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}


Value aliasnew(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "aliasnew <name>\n"
            "<name> name, 255 chars max."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);

    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;

    uint64 rand = GetRand((uint64)-1);
    vector<unsigned char> vchRand = CBigNum(rand).getvch();
    vector<unsigned char> vchToHash(vchRand);
    vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
    uint160 hash =  Hash160(vchToHash);

    CPubKey newDefaultKey;
    pwalletMain->GetKeyFromPool(newDefaultKey, false);
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    CScript scriptPubKey;
    scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_NEW) << hash << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;
    {
		LOCK(cs_main);
		EnsureWalletIsUnlocked();
		string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);
		mapMyNames[vchName] = wtx.GetHash();
    }
    printf("aliasnew : name=%s, rand=%s, tx=%s\n", stringFromVch(vchName).c_str(), HexStr(vchRand).c_str(), wtx.GetHash().GetHex().c_str());

    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(HexStr(vchRand));

    return res;
}


Value aliasactivate(const Array& params, bool fHelp) {
    if (fHelp || params.size() < 3 || params.size() > 4)
        throw runtime_error(
            "aliasactivate <alias> <rand> [<tx>] <value>\n"
            "Perform a first update after an aliasnew reservation.\n"
            "Note that the first update will go into a block 12 blocks after the aliasnew, at the soonest."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchRand = ParseHex(params[1].get_str());
    vector<unsigned char> vchValue;

    if (params.size() == 3)
        vchValue = vchFromValue(params[2]);
    else
        vchValue = vchFromValue(params[3]);

    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;

    {
	LOCK2(cs_main, pwalletMain->cs_wallet);

	if (mapNamePending.count(vchName) && mapNamePending[vchName].size()) {
		error("aliasactivate() : there are %d pending operations on that alias, including %s",
				(int)mapNamePending[vchName].size(),
				mapNamePending[vchName].begin()->GetHex().c_str());
		throw runtime_error("there are pending operations on that alias");
	}

	CTransaction tx;
	if (GetTxOfName(*pnamedb, vchName, tx)) {
		error("aliasactivate() : this alias is already active with tx %s",
				tx.GetHash().GetHex().c_str());
		throw runtime_error("this alias is already active");
	}

	EnsureWalletIsUnlocked();

	// Make sure there is a previous aliasnew tx on this name and that the random value matches
	uint256 wtxInHash;
	if (params.size() == 3) {
		if (!mapMyNames.count(vchName))
			throw runtime_error("could not find a coin with this alias, try specifying the aliasnew transaction id");
		wtxInHash = mapMyNames[vchName];
	}
	else  wtxInHash.SetHex(params[2].get_str());

	if (!pwalletMain->mapWallet.count(wtxInHash))
		throw runtime_error("previous transaction is not in the wallet");

    CPubKey newDefaultKey;
    pwalletMain->GetKeyFromPool(newDefaultKey, false);
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    CScript scriptPubKey;
	scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_ACTIVATE) << vchName << vchRand << vchValue << OP_2DROP << OP_2DROP;
	scriptPubKey += scriptPubKeyOrig;

	CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
	vector<unsigned char> vchHash;
	bool found = false;
	BOOST_FOREACH(CTxOut& out, wtxIn.vout) {
		vector<vector<unsigned char> > vvch;
		int op;
		if (DecodeNameScript(out.scriptPubKey, op, vvch)) {
			if (op != OP_ALIAS_NEW)
				throw runtime_error("previous transaction wasn't a aliasnew");
			vchHash = vvch[0];
			found = true;
			break;
		}
	}

	if (!found)
		throw runtime_error("previous tx on alias name is not an alias tx");

	vector<unsigned char> vchToHash(vchRand);
	vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
	uint160 hash =  Hash160(vchToHash);
	if (uint160(vchHash) != hash) {
		throw runtime_error("previous tx used a different random value");
	}

	int64 nNetFee = GetAliasNetworkFee(pindexBest->nHeight);

	// Round up to CENT
	nNetFee += CENT - 1;
	nNetFee = (nNetFee / CENT) * CENT;
	string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, nNetFee, wtxIn, wtx, false);
	if (strError != "")
		throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    return wtx.GetHash().GetHex();
}

Value aliasupdate(const Array& params, bool fHelp) {
    if (fHelp || 2 > params.size())
        throw runtime_error(
            "aliasupdate <alias> <value> [<toaddress>]\n"
            "Update and possibly transfer an alias."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue = vchFromValue(params[1]);

    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;
    CScript scriptPubKeyOrig;

    if (params.size() == 3) {
        string strAddress = params[2].get_str();
        uint160 hash160;
        bool isValid = AddressToHash160(strAddress.c_str(), hash160);
        if (!isValid) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid syscoin address");
        scriptPubKeyOrig.SetDestination(CBitcoinAddress(strAddress).Get());
    }
    else {
        CPubKey newDefaultKey;
        pwalletMain->GetKeyFromPool(newDefaultKey, false);
        scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    }

    CScript scriptPubKey;
    scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchName << vchValue << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    {
		LOCK2(cs_main, pwalletMain->cs_wallet);

        if (mapNamePending.count(vchName) && mapNamePending[vchName].size()) {
			error("aliasupdate() : there are %d pending operations on that alias, including %s",
					(int)mapNamePending[vchName].size(),
					mapNamePending[vchName].begin()->GetHex().c_str());
			throw runtime_error("there are pending operations on that alias");
		}

		EnsureWalletIsUnlocked();

		CTransaction tx;
		if (!GetTxOfName(*pnamedb, vchName, tx))
			throw runtime_error("could not find an alias with this name");

		uint256 wtxInHash = tx.GetHash();

        if (!pwalletMain->mapWallet.count(wtxInHash)) {
			error("aliasupdate() : this alias is not in your wallet %s",
					wtxInHash.GetHex().c_str());
			throw runtime_error("this alias is not in your wallet");
		}

		int64 nNetFee = GetAliasNetworkFee(pindexBest->nHeight);

		// Round up to CENT
		nNetFee += CENT - 1;
		nNetFee = (nNetFee / CENT) * CENT;
	
		CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
		string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, nNetFee, wtxIn, wtx, false);
		if (strError != "")
			throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

	return wtx.GetHash().GetHex();
}

Value aliaslist(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
                "aliaslist [<name>]\n"
                "list my own aliases"
                );

    vector<unsigned char> vchName;
    vector<unsigned char> vchLastName;

    if (params.size() == 1)
        vchName = vchFromValue(params[0]);

    vector<unsigned char> vchNameUniq;
    if (params.size() == 1)
        vchNameUniq = vchFromValue(params[0]);

    Array oRes;
    map< vector<unsigned char>, int > vNamesI;
    map< vector<unsigned char>, Object > vNamesO;

    {
    LOCK(pwalletMain->cs_wallet);

    CDiskTxPos txindex;
	uint256 hash;
	CTransaction tx;

	vector<unsigned char> vchValue;
	int nHeight;

	BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet)
	{
		hash = item.second.GetHash();
		if(!pblocktree->ReadTxIndex(hash, txindex))
			continue;

		if (tx.nVersion != SYSCOIN_TX_VERSION)
			continue;

		// name
		if(!GetNameOfTx(tx, vchName))
			continue;
		if(vchNameUniq.size() > 0 && vchNameUniq != vchName)
			continue;

		// value
		if(!GetValueOfNameTx(tx, vchValue))
			continue;

		// height
		nHeight = GetOfferTxPosHeight(txindex);

		Object oName;
		oName.push_back(Pair("name", stringFromVch(vchName)));
		oName.push_back(Pair("value", stringFromVch(vchValue)));
		if (!IsAliasMine(pwalletMain->mapWallet[tx.GetHash()]))
			oName.push_back(Pair("transferred", 1));
		string strAddress = "";
		GetNameAddress(tx, strAddress);
		oName.push_back(Pair("address", strAddress));
		oName.push_back(Pair("expires_in", nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
		if(nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
		{
			oName.push_back(Pair("expired", 1));
		}

		// get last active name only
		if(vNamesI.find(vchName) != vNamesI.end() && vNamesI[vchName] > nHeight)
			continue;

		vNamesI[vchName] = nHeight;
		vNamesO[vchName] = oName;
	}

    }

    BOOST_FOREACH(const PAIRTYPE(vector<unsigned char>, Object)& item, vNamesO)
        oRes.push_back(item.second);

    return oRes;
}

/**
 * [aliasshow description]
 * @param  params [description]
 * @param  fHelp  [description]
 * @return        [description]
 */
Value aliasshow(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "aliasshow <name>\n"
            "Show values of an alias.\n"
            );

    vector<unsigned char> vchName = vchFromValue(params[0]);
    CTransaction tx;
    Object oShowResult;
    
    {
    LOCK(pwalletMain->cs_wallet);

    // check for alias existence in DB
	vector<CNameIndex> vtxPos;
	if (!pnamedb->ReadName(vchName, vtxPos))
		throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from alias DB");
	if (vtxPos.size() < 1)
		throw JSONRPCError(RPC_WALLET_ERROR, "no result returned");

    // get transaction pointed to by alias
	uint256 blockHash;
	uint256 txHash = vtxPos.back().txHash;
	if (!GetTransaction(txHash, tx, blockHash, true))
		throw JSONRPCError(RPC_WALLET_ERROR, "failed to read transaction from disk");

	Object oName;
	vector<unsigned char> vchValue;
	int nHeight;
	
    uint256 hash;
	if (GetValueOfNameTxHash(txHash, vchValue, hash, nHeight)) {
		oName.push_back(Pair("name", stringFromVch(vchName)));
		string value = stringFromVch(vchValue);
		oName.push_back(Pair("value", value));
		oName.push_back(Pair("txid", tx.GetHash().GetHex()));
		string strAddress = "";
		GetNameAddress(tx, strAddress);
		oName.push_back(Pair("address", strAddress));
		oName.push_back(Pair("expires_in", nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
		if(nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0) {
			oName.push_back(Pair("expired", 1));
		}
		oShowResult = oName;
	}
    }
    return oShowResult;
}

/**
 * [aliashistory description]
 * @param  params [description]
 * @param  fHelp  [description]
 * @return        [description]
 */
Value aliashistory(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "aliashistory <name>\n"
            "List all stored values of an alias.\n");

    Array oRes;
    vector<unsigned char> vchName = vchFromValue(params[0]);
    string name = stringFromVch(vchName);

    {
    LOCK(pwalletMain->cs_wallet);

	vector<CNameIndex> vtxPos;
	if (!pnamedb->ReadName(vchName, vtxPos))
		throw JSONRPCError(RPC_WALLET_ERROR, "failed to read from alias DB");

	CNameIndex txPos2;
	uint256 txHash;
	uint256 blockHash;
	BOOST_FOREACH(txPos2, vtxPos) {
		txHash = txPos2.txHash;
		CTransaction tx;
		if (!GetTransaction(txHash, tx, blockHash, true)) {
			error("could not read txpos");
			continue;
		}

		Object oName;
		vector<unsigned char> vchValue;
		int nHeight;
		uint256 hash;
		if (GetValueOfNameTxHash(txHash, vchValue, hash, nHeight)) {
			oName.push_back(Pair("name", name));
			string value = stringFromVch(vchValue);
			oName.push_back(Pair("value", value));
			oName.push_back(Pair("txid", tx.GetHash().GetHex()));
			string strAddress = "";
			GetNameAddress(tx, strAddress);
			oName.push_back(Pair("address", strAddress));
			oName.push_back(Pair("expires_in", nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
			if(nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0) {
				oName.push_back(Pair("expired", 1));
			}
			oRes.push_back(oName);
		}
	}
    }
    return oRes;
}

/**
 * [aliasfilter description]
 * @param  params [description]
 * @param  fHelp  [description]
 * @return        [description]
 */
Value aliasfilter(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
                "aliasfilter [[[[[regexp] maxage=36000] from=0] nb=0] stat]\n"
                "scan and filter aliases\n"
                "[regexp] : apply [regexp] on aliases, empty means all aliases\n"
                "[maxage] : look in last [maxage] blocks\n"
                "[from] : show results from number [from]\n"
                "[nb] : show [nb] results, 0 means all\n"
                "[stat] : show some stats instead of results\n"
                "aliasfilter \"\" 5 # list aliases updated in last 5 blocks\n"
                "aliasfilter \"^name\" # list all aliases starting with \"name\"\n"
                "aliasfilter 36000 0 0 stat # display stats (number of names) on active aliases\n"
                );

    string strRegexp;
    int nFrom = 0;
    int nNb = 0;
    int nMaxAge = 36000;
    bool fStat = false;
    int nCountFrom = 0;
    int nCountNb = 0;


    if (params.size() > 0)
        strRegexp = params[0].get_str();

    if (params.size() > 1)
        nMaxAge = params[1].get_int();

    if (params.size() > 2)
        nFrom = params[2].get_int();

    if (params.size() > 3)
        nNb = params[3].get_int();

    if (params.size() > 4)
        fStat = (params[4].get_str() == "stat" ? true : false);


    //CNameDB dbName("r");
    Array oRes;

    vector<unsigned char> vchName;
    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!pnamedb->ScanNames(vchName, 100000000, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        string name = stringFromVch(pairScan.first);

        // regexp
        using namespace boost::xpressive;
        smatch nameparts;
        sregex cregex = sregex::compile(strRegexp);
        if(strRegexp != "" && !regex_search(name, nameparts, cregex))
            continue;

        CNameIndex txName = pairScan.second;
        int nHeight = txName.nHeight;

        // max age
        if(nMaxAge != 0 && pindexBest->nHeight - nHeight >= nMaxAge)
            continue;

        // from limits
        nCountFrom++;
        if(nCountFrom < nFrom + 1)
            continue;

        Object oName;
        oName.push_back(Pair("name", name));
        CTransaction tx;
        uint256 blockHash;
        uint256 txHash = txName.txHash;
        if ((nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
            || !GetTransaction(txHash, tx, blockHash, true))
        {
            oName.push_back(Pair("expired", 1));
        }
        else
        {
            vector<unsigned char> vchValue = txName.vValue;
            string value = stringFromVch(vchValue);
            oName.push_back(Pair("value", value));
            oName.push_back(Pair("expires_in", nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
        }
        oRes.push_back(oName);

        nCountNb++;
        // nb limits
        if(nNb > 0 && nCountNb >= nNb)
            break;
    }

    if(fStat)
    {
        Object oStat;
        oStat.push_back(Pair("blocks",    (int)nBestHeight));
        oStat.push_back(Pair("count",     (int)oRes.size()));
        //oStat.push_back(Pair("sha256sum", SHA256(oRes), true));
        return oStat;
    }

    return oRes;
}

/**
 * [aliasscan description]
 * @param  params [description]
 * @param  fHelp  [description]
 * @return        [description]
 */
Value aliasscan(const Array& params, bool fHelp)
{
    if (fHelp || 2 > params.size())
        throw runtime_error(
                "aliasscan [<start-name>] [<max-returned>]\n"
                "scan all aliases, starting at start-name and returning a maximum number of entries (default 500)\n"
                );

    vector<unsigned char> vchName;
    int nMax = 500;
    if (params.size() > 0)
    {
        vchName = vchFromValue(params[0]);
    }

    if (params.size() > 1)
    {
        Value vMax = params[1];
        ConvertTo<double>(vMax);
        nMax = (int)vMax.get_real();
    }

    //CNameDB dbName("r");
    Array oRes;

    vector<pair<vector<unsigned char>, CNameIndex> > nameScan;
    if (!pnamedb->ScanNames(vchName, nMax, nameScan))
        throw JSONRPCError(RPC_WALLET_ERROR, "scan failed");

    pair<vector<unsigned char>, CNameIndex> pairScan;
    BOOST_FOREACH(pairScan, nameScan)
    {
        Object oName;
        string name = stringFromVch(pairScan.first);
        oName.push_back(Pair("name", name));
        CTransaction tx;
        CNameIndex txName = pairScan.second;
        uint256 blockHash;

        //CDiskTxPos txPos = pairScan.second;
        //int nHeight = GetTxPosHeight(txPos);
        int nHeight = txName.nHeight;
        vector<unsigned char> vchValue = txName.vValue;
        if ((nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight <= 0)
            || !GetTransaction(txName.txHash, tx, blockHash, true))
        {
            oName.push_back(Pair("expired", 1));
        }
        else
        {
            string value = stringFromVch(vchValue);
            //string strAddress = "";
            //GetNameAddress(tx, strAddress);
            oName.push_back(Pair("value", value));
            //oName.push_back(Pair("txid", tx.GetHash().GetHex()));
            //oName.push_back(Pair("address", strAddress));
            oName.push_back(Pair("expires_in", nHeight + GetNameDisplayExpirationDepth(nHeight) - pindexBest->nHeight));
        }
        oRes.push_back(oName);
    }

    return oRes;
}

/*

Value aliasclean(const Array& params, bool fHelp) {
    if (fHelp || params.size())
        throw runtime_error("aliasclean\nClean unsatisfiable alias transactions from the wallet - including aliasactivate on an already taken alias\n");
    {
        LOCK2(cs_main,pwalletMain->cs_wallet);
        map<uint256, CWalletTx> mapRemove;

        printf("-----------------------------\n");
        {
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet) {
                CWalletTx& wtx = item.second;
                vector<unsigned char> vchName;
                if (wtx.GetDepthInMainChain() < 1 && IsConflictedNameTx(pblocktree, wtx, vchName)) {
                    uint256 hash = wtx.GetHash();
                    mapRemove[hash] = wtx;
                }
            }
        }

        bool fRepeat = true;
        while (fRepeat) {
            fRepeat = false;
            BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, pwalletMain->mapWallet) {
                CWalletTx& wtx = item.second;
                BOOST_FOREACH(const CTxIn& txin, wtx.vin) {
                    uint256 hash = wtx.GetHash();

                    // If this tx depends on a tx to be removed, remove it too
                    if (mapRemove.count(txin.prevout.hash) && !mapRemove.count(hash)) {
                        mapRemove[hash] = wtx;
                        fRepeat = true;
                    }
                }
            }
        }

        BOOST_FOREACH(PAIRTYPE(const uint256, CWalletTx)& item, mapRemove) {
            CWalletTx& wtx = item.second;

            UnspendInputs(wtx);
            wtx.RemoveFromMemoryPool();
            pwalletMain->EraseFromWallet(wtx.GetHash());
            vector<unsigned char> vchName;
            if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName)) {
                string name = stringFromVch(vchName);
                printf("name_clean() : erase %s from pending of name %s", 
                        wtx.GetHash().GetHex().c_str(), name.c_str());
                if (!mapNamePending[vchName].erase(wtx.GetHash()))
                    error("name_clean() : erase but it was not pending");
            }
            wtx.print();
        }
        printf("-----------------------------\n");
    }
    return true;
}

Value deletetransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
                "deletetransaction <txid>\nNormally used when a transaction cannot be confirmed due to a double spend.\nRestart the program after executing this call.\n"
                );

    {
      LOCK2(cs_main,pwalletMain->cs_mapWallet);
      
      // look for txn in wallet
      uint256 hash;
      hash.SetHex(params[0].get_str());
      if (!pwalletMain->mapWallet.count(hash))
        throw runtime_error("transaction not in wallet");

      if (!mapTransactions.count(hash)) {
        //throw runtime_error("transaction not in memory - is already in blockchain?");
        CTransaction tx;
        uint256 hashBlock = 0;
        if (GetTransaction(hash, tx, hashBlock) && hashBlock != 0)
          throw runtime_error("transaction is already in blockchain");
      }
      CWalletTx wtx = pwalletMain->mapWallet[hash];
      UnspendInputs(wtx);

      // We are not removing from mapTransactions because this can cause memory corruption
      // during mining.  The user should restart to clear the tx from memory.
      wtx.RemoveFromMemoryPool();
      pwalletMain->EraseFromWallet(wtx.GetHash());
      vector<unsigned char> vchName;
      if (GetNameOfTx(wtx, vchName) && mapNamePending.count(vchName)) {
        printf("deletetransaction() : remove from pending");
        mapNamePending[vchName].erase(wtx.GetHash());
      }
      return "success, please restart program to clear memory";
    }
}

*/

Value phrpcfunc(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "placeholder <>\n"
            "<> TBD."
            + HelpRequiringPassphrase());

return 0;
}

Value datanew(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "datanew <alias>\n"
            "<alias> data alias name, 255 chars max."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);

    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;

    uint64 rand = GetRand((uint64)-1);
    vector<unsigned char> vchRand = CBigNum(rand).getvch();
    vector<unsigned char> vchToHash(vchRand);
    vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
    uint160 hash =  Hash160(vchToHash);

    CPubKey newDefaultKey;
    pwalletMain->GetKeyFromPool(newDefaultKey, false);
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    CScript scriptPubKey;
    scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_NEW) << hash << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig; {
        LOCK(cs_main);
        EnsureWalletIsUnlocked();
        string strError = pwalletMain->SendMoney(scriptPubKey, MIN_AMOUNT, wtx, false);
        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        mapMyNames[vchName] = wtx.GetHash();
    }
    printf("datanew : name=%s, rand=%s, tx=%s\n", stringFromVch(vchName).c_str(), HexStr(vchRand).c_str(), wtx.GetHash().GetHex().c_str());

    vector<Value> res;
    res.push_back(wtx.GetHash().GetHex());
    res.push_back(HexStr(vchRand));

    return res;
}

Value dataactivate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 4)
        throw runtime_error(
            "dataactivate <name> <rand> [<tx>] <data>\n"
            "Perform a data firstupdate after a datanew reservation.\n"
            "Note that the firstupdate will go into a block 12 blocks after the datanew, at the soonest."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchRand = ParseHex(params[1].get_str());
    vector<unsigned char> vchValue;
	string baSig;

    // Transaction data
    std::string txdata;
	if (params.size() == 4)
		txdata = params[3].get_str();
	else
		txdata = params[2].get_str();
	if (txdata.length() > MAX_TX_DATA_SIZE)
		throw JSONRPCError(RPC_INVALID_PARAMETER, "Data chunk is too long.  Split the payload to several transactions.");

    // sign using the first key in wallet
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first)) {
            // sign the data and store it as the alias value
            CKeyID keyID;
            CBitcoinAddress address;
            address.Set(entry.first);
            if (!address.GetKeyID(keyID))
                throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
            CKey key;
            if (!pwalletMain->GetKey(keyID, key))
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
            CHashWriter ss(SER_GETHASH, 0);
            ss << strMessageMagic;
            ss << txdata;
            vector<unsigned char> vchSig;
            if (!key.SignCompact(ss.GetHash(), vchSig))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
            baSig = EncodeBase64(vchSig.data(), vchSig.size());
            vchValue = vchFromString(baSig);
            break;
        }
    }

    // this is a syscoin transaction
    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;

    {
    LOCK(cs_main);
    if (mapNamePending.count(vchName) && mapNamePending[vchName].size()) {
        error("dataactivate() : there are %d pending operations on that data, including %s",
                (int)mapNamePending[vchName].size(),
                mapNamePending[vchName].begin()->GetHex().c_str());
        throw runtime_error("there are pending operations on that data");
    }

    CTransaction tx;
    if (GetTxOfName(*pnamedb, vchName, tx)) {
        error("dataactivate() : this data is already active with tx %s",
                tx.GetHash().GetHex().c_str());
        throw runtime_error("this data is already active");
    }

    {
    EnsureWalletIsUnlocked();

    // Make sure there is a previous aliasnew tx on this name and that the random value matches
    uint256 wtxInHash;
    if (params.size() == 3) {
        if (!mapMyNames.count(vchName)) 
            throw runtime_error("could not find any data with this name, try specifying the datanew transaction id");
        wtxInHash = mapMyNames[vchName];
    }
    else  wtxInHash.SetHex(params[2].get_str());

    if (!pwalletMain->mapWallet.count(wtxInHash))
        throw runtime_error("previous transaction is not in the wallet");

    CPubKey newDefaultKey;
    pwalletMain->GetKeyFromPool(newDefaultKey, false);
    CScript scriptPubKeyOrig;
    scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    // create a syscoin DATA_FIRSTUPDATE transaction
    CScript scriptPubKey;
    scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_ACTIVATE) << vchName << vchRand << vchValue << OP_2DROP << OP_2DROP;
    scriptPubKey += scriptPubKeyOrig;

    CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
    vector<unsigned char> vchHash;
    bool found = false;
    BOOST_FOREACH(CTxOut& out, wtxIn.vout) {
        vector<vector<unsigned char> > vvch;
        int op;
        if (DecodeNameScript(out.scriptPubKey, op, vvch)) {
            if (op != OP_ALIAS_NEW)
                throw runtime_error("previous transaction wasn't a datanew");
            vchHash = vvch[0];
            found = true;
            break;
        }
    }

    if (!found)
        throw runtime_error("previous tx on this data is not a syscoin tx");

    vector<unsigned char> vchToHash(vchRand);
    vchToHash.insert(vchToHash.end(), vchName.begin(), vchName.end());
    uint160 hash =  Hash160(vchToHash);
    if (uint160(vchHash) != hash)
        throw runtime_error("previous tx used a different random value");

    int64 nNetFee = GetAliasNetworkFee(pindexBest->nHeight);

    // Round up to CENT
    nNetFee += CENT - 1;
    nNetFee = (nNetFee / CENT) * CENT;
    string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, nNetFee, wtxIn, wtx, false, txdata);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    }
    baSig += "\n" + wtx.GetHash().GetHex();
    return baSig;
}

Value dataupdate(const Array& params, bool fHelp)
{
    if (fHelp || 2 > params.size())
        throw runtime_error(
            "dataupdate <name> <data> [<toaddress>] [<encrypt=false>]\n"
            "Update and possibly transfer some data."
            + HelpRequiringPassphrase());

    vector<unsigned char> vchName = vchFromValue(params[0]);
    vector<unsigned char> vchValue;
    string baSig;

    // Transaction data
    std::string txdata = params[1].get_str();
	if (txdata.length() > MAX_TX_DATA_SIZE)
		throw JSONRPCError(RPC_INVALID_PARAMETER, "Data chunk is too long.  Split the payload to several transactions.");

    // sign using the first key in wallet
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first)) {
            // sign the data and store it as the alias value
            CKeyID keyID;
            CBitcoinAddress address;
            address.Set(entry.first);
            if (!address.GetKeyID(keyID))
                throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
            CKey key;
            if (!pwalletMain->GetKey(keyID, key))
                throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
            CHashWriter ss(SER_GETHASH, 0);
            ss << strMessageMagic;
            ss << txdata;
            vector<unsigned char> vchSig;
            if (!key.SignCompact(ss.GetHash(), vchSig))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");
            baSig = EncodeBase64(vchSig.data(), vchSig.size());
            vchValue = vchFromString(baSig);
        }
    }

    CWalletTx wtx;
    wtx.nVersion = SYSCOIN_TX_VERSION;
    CScript scriptPubKeyOrig;

    if (params.size() == 3) {
        string strAddress = params[2].get_str();
        uint160 hash160;
        bool isValid = AddressToHash160(strAddress.c_str(), hash160);
        if (!isValid) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid syscoin address");
        scriptPubKeyOrig.SetDestination(CBitcoinAddress(strAddress).Get());
    }
    else {
        CPubKey newDefaultKey;
        pwalletMain->GetKeyFromPool(newDefaultKey, false);
        scriptPubKeyOrig.SetDestination(newDefaultKey.GetID());
    }

    // create a syscoind DATA_UPDATE transaction
    CScript scriptPubKey;
    scriptPubKey << CScript::EncodeOP_N(OP_ALIAS_UPDATE) << vchName << vchValue << OP_2DROP << OP_DROP;
    scriptPubKey += scriptPubKeyOrig;

    {
    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (mapNamePending.count(vchName) && mapNamePending[vchName].size()) {
        error("dataupdate() : there are %d pending operations on that data, including %s",
                (int)mapNamePending[vchName].size(),
                mapNamePending[vchName].begin()->GetHex().c_str());
        throw runtime_error("there are pending operations on that data");
    }

    EnsureWalletIsUnlocked();

    CTransaction tx;
    if (!GetTxOfName(*pnamedb, vchName, tx))
        throw runtime_error("could not find this data"
        		"+-in your wallet");

    uint256 wtxInHash = tx.GetHash();

    if (!pwalletMain->mapWallet.count(wtxInHash)) {
        error("aliasupdate() : this data is not in your wallet %s",
                wtxInHash.GetHex().c_str());
        throw runtime_error("this data is not in your wallet");
    }

    CWalletTx& wtxIn = pwalletMain->mapWallet[wtxInHash];
    string strError = SendMoneyWithInputTx(scriptPubKey, MIN_AMOUNT, 0, wtxIn, wtx, false);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    }

    baSig += "\n" + wtx.GetHash().GetHex();
    return baSig;
}

Value datalist(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
                "datalist [<alias>]\n"
                "list my own data"
                );
    return (double)0;
}

Value datashow(const Array& params, bool fHelp) {
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "datashow <alias>\n"
            "Show data tied to alias.\n"
            );
    return(double)0;
}

Value datahistory(const Array& params, bool fHelp)
{
    if (fHelp || 1 != params.size())
        throw runtime_error(
            "datahistory <alias>\n"
            "List all stored data of alias.\n");
    return (double)0;
}

Value datafilter(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 5)
        throw runtime_error(
                "datafilter [[[[[regexp] maxage=36000] from=0] nb=0] stat]\n"
                "scan and filter data\n"
                "[regexp] : apply [regexp] on data, empty means all data\n"
                "[maxage] : look in last [maxage] blocks\n"
                "[from] : show results from number [from]\n"
                "[nb] : show [nb] results, 0 means all\n"
                "[stats] : show some stats instead of results\n"
                "datafilter \"\" 5 # list data updated in last 5 blocks\n"
                "datafilter \"^name\" # list all data starting with \"name\"\n"
                "datafilter 36000 0 0 stat # display stats (number of data aliases) on active data\n"
                );
    return(double)0;
}

Value keyscan(const Array& params, bool fHelp)
{
    if (fHelp || 2 > params.size())
        throw runtime_error(
                "keyscan [<start-name>] [<max-returned>]\n"
                "scan all keys, starting at start-name and returning a maximum number of entries (default 500)\n"
                );
    return(double)0;
}

Value listtransactions(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listtransactions [account] [count=10] [from=0]\n"
            "Returns up to [count] most recent transactions skipping the first [from] transactions for account [account].");

    string strAccount = "*";
    if (params.size() > 0)
        strAccount = params[0].get_str();
    int nCount = 10;
    if (params.size() > 1)
        nCount = params[1].get_int();
    int nFrom = 0;
    if (params.size() > 2)
        nFrom = params[2].get_int();

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    Array ret;

    std::list<CAccountingEntry> acentries;
    CWallet::TxItems txOrdered = pwalletMain->OrderedTxItems(acentries, strAccount);

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(*pwtx, strAccount, 0, true, ret);
        CAccountingEntry *const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount+nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;
    Array::iterator first = ret.begin();
    std::advance(first, nFrom);
    Array::iterator last = ret.begin();
    std::advance(last, nFrom+nCount);

    if (last != ret.end()) ret.erase(last, ret.end());
    if (first != ret.begin()) ret.erase(ret.begin(), first);

    std::reverse(ret.begin(), ret.end()); // Return oldest to newest

    return ret;
}

Value listaccounts(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "listaccounts [minconf=1]\n"
            "Returns Object that has account names as keys, account balances as values.");

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    map<string, int64> mapAccountBalances;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first)) // This address belongs to me
            mapAccountBalances[entry.second] = 0;
    }

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        int64 nFee;
        string strSentAccount;
        list<pair<CTxDestination, int64> > listReceived;
        list<pair<CTxDestination, int64> > listSent;
        bool fNameTx;
        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, fNameTx);
        mapAccountBalances[strSentAccount] -= nFee;
        BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& s, listSent)
            mapAccountBalances[strSentAccount] -= s.second;
        if (wtx.GetDepthInMainChain() >= nMinDepth)
        {
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& r, listReceived)
                if (pwalletMain->mapAddressBook.count(r.first))
                    mapAccountBalances[pwalletMain->mapAddressBook[r.first]] += r.second;
                else
                    mapAccountBalances[""] += r.second;
        }
    }

    list<CAccountingEntry> acentries;
    CWalletDB(pwalletMain->strWalletFile).ListAccountCreditDebit("*", acentries);
    BOOST_FOREACH(const CAccountingEntry& entry, acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    Object ret;
    BOOST_FOREACH(const PAIRTYPE(string, int64)& accountBalance, mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

Value listsinceblock(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listsinceblock [blockhash] [target-confirmations]\n"
            "Get all transactions in blocks since block [blockhash], or all transactions if omitted");

    CBlockIndex *pindex = NULL;
    int target_confirms = 1;

    if (params.size() > 0)
    {
        uint256 blockId = 0;

        blockId.SetHex(params[0].get_str());
        pindex = CBlockLocator(blockId).GetBlockIndex();
    }

    if (params.size() > 1)
    {
        target_confirms = params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    int depth = pindex ? (1 + nBestHeight - pindex->nHeight) : -1;

    Array transactions;

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++)
    {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth)
            ListTransactions(tx, "*", 0, true, transactions);
    }

    uint256 lastblock;

    if (target_confirms == 1)
    {
        lastblock = hashBestChain;
    }
    else
    {
        int target_height = pindexBest->nHeight + 1 - target_confirms;

        CBlockIndex *block;
        for (block = pindexBest;
             block && block->nHeight > target_height;
             block = block->pprev)  { }

        lastblock = block ? block->GetBlockHash() : 0;
    }

    Object ret;
    ret.push_back(Pair("transactions", transactions));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

Value gettransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "gettransaction <txid>\n"
            "Get detailed information about in-wallet transaction <txid>");

    uint256 hash;
    hash.SetHex(params[0].get_str());

    Object entry;
    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwalletMain->mapWallet[hash];

    int64 nCredit = wtx.GetCredit();
    int64 nDebit = wtx.GetDebit();
    int64 nNet = nCredit - nDebit;
    int64 nFee = (wtx.IsFromMe() ? wtx.GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe())
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(wtx, entry);

    Array details;
    ListTransactions(wtx, "*", 0, false, details);
    entry.push_back(Pair("details", details));

    return entry;
}


Value backupwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "backupwallet <destination>\n"
            "Safely copies wallet.dat to destination, which can be a directory or a path with filename.");

    string strDest = params[0].get_str();
    if (!BackupWallet(*pwalletMain, strDest))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");

    return Value::null;
}


Value keypoolrefill(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "keypoolrefill\n"
            "Fills the keypool."
            + HelpRequiringPassphrase());

    EnsureWalletIsUnlocked();

    pwalletMain->TopUpKeyPool();

    if (pwalletMain->GetKeyPoolSize() < GetArg("-keypool", 100))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return Value::null;
}


void ThreadTopUpKeyPool(void* parg)
{
    // Make this thread recognisable as the key-topping-up thread
    RenameThread("syscoin-key-top");

    pwalletMain->TopUpKeyPool();
}

void ThreadCleanWalletPassphrase(void* parg)
{
    // Make this thread recognisable as the wallet relocking thread
    RenameThread("syscoin-lock-wa");

    int64 nMyWakeTime = GetTimeMillis() + *((int64*)parg) * 1000;

    ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

    if (nWalletUnlockTime == 0)
    {
        nWalletUnlockTime = nMyWakeTime;

        do
        {
            if (nWalletUnlockTime==0)
                break;
            int64 nToSleep = nWalletUnlockTime - GetTimeMillis();
            if (nToSleep <= 0)
                break;

            LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);
            MilliSleep(nToSleep);
            ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

        } while(1);

        if (nWalletUnlockTime)
        {
            nWalletUnlockTime = 0;
            pwalletMain->Lock();
        }
    }
    else
    {
        if (nWalletUnlockTime < nMyWakeTime)
            nWalletUnlockTime = nMyWakeTime;
    }

    LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);

    delete (int64*)parg;
}

Value walletpassphrase(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout>\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    if (!pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked.");

    // Note that the walletpassphrase is stored in params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() > 0)
    {
        if (!pwalletMain->Unlock(strWalletPass))
            throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }
    else
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout>\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");

    NewThread(ThreadTopUpKeyPool, NULL);
    int64* pnSleepTime = new int64(params[1].get_int64());
    NewThread(ThreadCleanWalletPassphrase, pnSleepTime);

    return Value::null;
}


Value walletpassphrasechange(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return Value::null;
}


Value walletlock(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 0))
        throw runtime_error(
            "walletlock\n"
            "Removes the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    {
        LOCK(cs_nWalletUnlockTime);
        pwalletMain->Lock();
        nWalletUnlockTime = 0;
    }

    return Value::null;
}


Value encryptwallet(const Array& params, bool fHelp)
{
    if (!pwalletMain->IsCrypted() && (fHelp || params.size() != 1))
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");
    if (fHelp)
        return true;
    if (pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwalletMain->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "wallet encrypted; SysCoin server stopping, restart to run with encrypted wallet. The keypool has been flushed, you need to make a new backup.";
}

class DescribeAddressVisitor : public boost::static_visitor<Object>
{
public:
    Object operator()(const CNoDestination &dest) const { return Object(); }

    Object operator()(const CKeyID &keyID) const {
        Object obj;
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        obj.push_back(Pair("isscript", false));
        obj.push_back(Pair("pubkey", HexStr(vchPubKey)));
        obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        return obj;
    }

    Object operator()(const CScriptID &scriptID) const {
        Object obj;
        obj.push_back(Pair("isscript", true));
        CScript subscript;
        pwalletMain->GetCScript(scriptID, subscript);
        std::vector<CTxDestination> addresses;
        txnouttype whichType;
        int nRequired;
        ExtractDestinations(subscript, whichType, addresses, nRequired);
        obj.push_back(Pair("script", GetTxnOutputType(whichType)));
        Array a;
        BOOST_FOREACH(const CTxDestination& addr, addresses)
            a.push_back(CBitcoinAddress(addr).ToString());
        obj.push_back(Pair("addresses", a));
        if (whichType == TX_MULTISIG)
            obj.push_back(Pair("sigsrequired", nRequired));
        return obj;
    }
};

Value validateaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "validateaddress <syscoinaddress>\n"
            "Return information about <syscoinaddress>.");

    CBitcoinAddress address(params[0].get_str());
    bool isValid = address.IsValid();

    Object ret;
    ret.push_back(Pair("isvalid", isValid));
    if (isValid)
    {
        CTxDestination dest = address.Get();
        string currentAddress = address.ToString();
        ret.push_back(Pair("address", currentAddress));
        bool fMine = pwalletMain ? IsMine(*pwalletMain, dest) : false;
        ret.push_back(Pair("ismine", fMine));
        if (fMine) {
            Object detail = boost::apply_visitor(DescribeAddressVisitor(), dest);
            ret.insert(ret.end(), detail.begin(), detail.end());
        }
        if (pwalletMain && pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest]));
    }
    return ret;
}

Value lockunspent(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "lockunspent unlock? [array-of-Objects]\n"
            "Updates list of temporarily unspendable outputs.");

    if (params.size() == 1)
        RPCTypeCheck(params, list_of(bool_type));
    else
        RPCTypeCheck(params, list_of(bool_type)(array_type));

    bool fUnlock = params[0].get_bool();

    if (params.size() == 1) {
        if (fUnlock)
            pwalletMain->UnlockAllCoins();
        return true;
    }

    Array outputs = params[1].get_array();
    BOOST_FOREACH(Value& output, outputs)
    {
        if (output.type() != obj_type)
            throw JSONRPCError(-8, "Invalid parameter, expected object");
        const Object& o = output.get_obj();

        RPCTypeCheck(o, map_list_of("txid", str_type)("vout", int_type));

        string txid = find_value(o, "txid").get_str();
        if (!IsHex(txid))
            throw JSONRPCError(-8, "Invalid parameter, expected hex txid");

        int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0)
            throw JSONRPCError(-8, "Invalid parameter, vout must be positive");

        COutPoint outpt(uint256(txid), nOutput);

        if (fUnlock)
            pwalletMain->UnlockCoin(outpt);
        else
            pwalletMain->LockCoin(outpt);
    }

    return true;
}

Value listlockunspent(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "listlockunspent\n"
            "Returns list of temporarily unspendable outputs.");

    vector<COutPoint> vOutpts;
    pwalletMain->ListLockedCoins(vOutpts);

    Array ret;

    BOOST_FOREACH(COutPoint &outpt, vOutpts) {
        Object o;

        o.push_back(Pair("txid", outpt.hash.GetHex()));
        o.push_back(Pair("vout", (int)outpt.n));
        ret.push_back(o);
    }

    return ret;
}

