// Copyright 2018 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wallet.h"
#include <boost/uuid/uuid.hpp>

#include "core/ecc_native.h"
#include "core/block_crypt.h"
#include "utility/logger.h"
#include "utility/helpers.h"
#include "swap_transaction.h"
#include <algorithm>
#include <random>
#include <iomanip>

namespace std
{
    string to_string(const beam::WalletID& id)
    {
        static_assert(sizeof(id) == sizeof(id.m_Channel) + sizeof(id.m_Pk), "");

		char szBuf[sizeof(id) * 2 + 1];
		beam::to_hex(szBuf, &id, sizeof(id));

		const char* szPtr = szBuf;
		while (*szPtr == '0')
			szPtr++;

		if (!*szPtr)
			szPtr--; // leave at least 1 symbol

		return szPtr;
	}
}

namespace beam
{
    using namespace wallet;
    using namespace std;
    using namespace ECC;

    int WalletID::cmp(const WalletID& x) const
    {
        int n = m_Channel.cmp(x.m_Channel);
        if (n)
            return n;
        return m_Pk.cmp(x.m_Pk);
    }

    bool WalletID::FromBuf(const ByteBuffer& x)
    {
        if (x.size() > sizeof(*this))
            return false;

        typedef uintBig_t<sizeof(*this)> BigSelf;
        static_assert(sizeof(BigSelf) == sizeof(*this), "");

        *reinterpret_cast<BigSelf*>(this) = Blob(x);
        return true;
    }

    bool WalletID::FromHex(const std::string& s)
    {
        bool bValid = true;
        ByteBuffer bb = from_hex(s, &bValid);

        return bValid && FromBuf(bb);
    }

    bool WalletID::IsValid() const
    {
        ECC::Point::Native p;
        return proto::ImportPeerID(p, m_Pk);
    }

    std::ostream& operator<<(std::ostream& os, const TxID& uuid)
    {
        os << "[" << to_hex(uuid.data(), uuid.size()) << "]";
        return os;
    }

    std::ostream& operator<<(std::ostream& os, const PrintableAmount& amount)
    {
        auto width = os.width();

        if (amount.m_showPoint)
        {
            os << setw(width - 1) << Amount(amount.m_value / Rules::Coin)
                << "."
                << (amount.m_value % Rules::Coin);
            return os;
        }
        
        if (amount.m_value >= Rules::Coin)
        {
            os << setw(width) << Amount(amount.m_value / Rules::Coin);
        }
        Amount c = amount.m_value % Rules::Coin;
        if (c > 0 || amount.m_value == 0)
        {
            os << setw(width) << c;
        }
        return os;
    }

    const char Wallet::s_szNextUtxoEvt[] = "NextUtxoEvent";

    Wallet::Wallet(IWalletDB::Ptr walletDB, TxCompletedAction&& action)
        : m_WalletDB{ walletDB }
        , m_pNodeNetwork(nullptr)
        , m_pWalletNetwork(nullptr)
        , m_tx_completed_action{move(action)}
        , m_LastSyncTotal(0)
        , m_OwnedNodesOnline(0)
    {
        assert(walletDB);
        resume_all_tx();
    }

    void Wallet::get_Kdf(Key::IKdf::Ptr& pKdf)
    {
        pKdf = m_WalletDB->get_MasterKdf();
    }

    void Wallet::OnOwnedNode(const PeerID& id, bool bUp)
    {
        if (bUp)
        {
            if (!m_OwnedNodesOnline++)
                RequestUtxoEvents(); // maybe time to refresh
        }
        else
        {
            assert(m_OwnedNodesOnline);
            if (!--m_OwnedNodesOnline)
                AbortUtxoEvents();
        }
    }

    Block::SystemState::IHistory& Wallet::get_History()
    {
        return m_WalletDB->get_History();
    }

    void Wallet::set_Network(proto::FlyClient::INetwork& netNode, IWalletNetwork& netWallet)
    {
        m_pNodeNetwork = &netNode;
        m_pWalletNetwork = &netWallet;
    }

    Wallet::~Wallet()
    {
        // clear all requests
#define THE_MACRO(type, msgOut, msgIn) \
        while (!m_Pending##type.empty()) \
            DeleteReq(*m_Pending##type.begin());

        REQUEST_TYPES_All(THE_MACRO)
#undef THE_MACRO
    }

    TxID Wallet::transfer_money(const WalletID& from, const WalletID& to, Amount amount, Amount fee, bool sender, ByteBuffer&& message)
    {
        auto txID = wallet::GenerateTxID();
        auto tx = constructTransaction(txID, TxType::Simple);
        Height currentHeight = m_WalletDB->getCurrentHeight();

        tx->SetParameter(TxParameterID::TransactionType, TxType::Simple, false);
        tx->SetParameter(TxParameterID::MaxHeight, currentHeight + 1440, false); // transaction is valid +24h from now
        tx->SetParameter(TxParameterID::IsInitiator, true, false);

        TxDescription txDescription;

        txDescription.m_txId = txID;
        txDescription.m_amount = amount;
        txDescription.m_fee = fee;
        txDescription.m_minHeight = currentHeight;
        txDescription.m_peerId = to;
        txDescription.m_myId = from;
        txDescription.m_message = move(message);
        txDescription.m_createTime = getTimestamp();
        txDescription.m_sender = sender;
        txDescription.m_status = TxStatus::Pending;
        m_WalletDB->saveTx(txDescription);

        m_transactions.emplace(txID, tx);

        updateTransaction(txID);

        return txID;
    }

    TxID Wallet::swap_coins(const WalletID& from, const WalletID& to, Amount amount, Amount fee, wallet::AtomicSwapCoin swapCoin, Amount swapAmount)
    {
        auto txID = wallet::GenerateTxID();
        auto tx = constructTransaction(txID, TxType::AtomicSwap);

        tx->SetParameter(TxParameterID::TransactionType, TxType::AtomicSwap, false);
        tx->SetParameter(TxParameterID::CreateTime, getTimestamp(), false);
        tx->SetParameter(TxParameterID::Amount, amount, false);
        tx->SetParameter(TxParameterID::Fee, fee, false);
        tx->SetParameter(TxParameterID::MinHeight, m_WalletDB->getCurrentHeight(), false);
        tx->SetParameter(TxParameterID::PeerID, to, false);
        tx->SetParameter(TxParameterID::MyID, from, false);
        tx->SetParameter(TxParameterID::IsSender, true, false);
        tx->SetParameter(TxParameterID::IsInitiator, true, false);
        tx->SetParameter(TxParameterID::Status, TxStatus::Pending, true);

        tx->SetParameter(TxParameterID::AtomicSwapCoin, swapCoin, false);
        tx->SetParameter(TxParameterID::AtomicSwapAmount, swapAmount, false);

        m_transactions.emplace(txID, tx);

        updateTransaction(txID);

        return txID;
    }

    void Wallet::resume_tx(const TxDescription& tx)
    {
        if (tx.canResume() && m_transactions.find(tx.m_txId) == m_transactions.end())
        {
            auto t = constructTransaction(tx.m_txId, TxType::Simple);

            m_transactions.emplace(tx.m_txId, t);
        }
    }

    void Wallet::resume_all_tx()
    {
        auto txs = m_WalletDB->getTxHistory();
        for (auto& tx : txs)
        {
            resume_tx(tx);
        }
    }

    void Wallet::on_tx_completed(const TxID& txID)
    {
        auto it = m_transactions.find(txID);
        if (it != m_transactions.end())
        {
            m_transactions.erase(it);
        }
 
        if (m_tx_completed_action)
        {
            m_tx_completed_action(txID);
        }
    }

    void Wallet::confirm_outputs(const vector<Coin>& coins)
    {
        for (auto& coin : coins)
            getUtxoProof(coin.m_ID);
    }

    bool Wallet::MyRequestUtxo::operator < (const MyRequestUtxo& x) const
    {
        return m_Msg.m_Utxo < x.m_Msg.m_Utxo;
    }

    bool Wallet::MyRequestKernel::operator < (const MyRequestKernel& x) const
    {
        return m_TxID < x.m_TxID;
    }

    bool Wallet::MyRequestTransaction::operator < (const MyRequestTransaction& x) const
    {
        return m_TxID < x.m_TxID;
    }

    bool Wallet::MyRequestUtxoEvents::operator < (const MyRequestUtxoEvents& x) const
    {
        return false;
    }

    bool Wallet::MyRequestBbsChannel::operator < (const MyRequestBbsChannel &x) const
    {
        return false;
    }

    void Wallet::RequestHandler::OnComplete(Request& r)
    {
        uint32_t n = get_ParentObj().SyncRemains();

        switch (r.get_Type())
        {
#define THE_MACRO(type, msgOut, msgIn) \
        case Request::Type::type: \
            { \
                MyRequest##type& x = static_cast<MyRequest##type&>(r); \
                get_ParentObj().DeleteReq(x); \
                get_ParentObj().OnRequestComplete(x); \
            } \
            break;

        REQUEST_TYPES_All(THE_MACRO)
#undef THE_MACRO

        default:
            assert(false);
        }

        if (n)
            get_ParentObj().CheckSyncDone();
    }

    void Wallet::confirm_kernel(const TxID& txID, const TxKernel& kernel)
    {
        if (auto it = m_transactions.find(txID); it != m_transactions.end())
        {
            MyRequestKernel::Ptr pVal(new MyRequestKernel);
            pVal->m_TxID = txID;
            kernel.get_ID(pVal->m_Msg.m_ID);

            if (PostReqUnique(*pVal))
                LOG_INFO() << "Get proof for kernel: " << pVal->m_Msg.m_ID;
        }
    }

    bool Wallet::get_tip(Block::SystemState::Full& state) const
    {
        return m_WalletDB->get_History().get_Tip(state);
    }

    void Wallet::send_tx_params(const WalletID& peerID, SetTxParameter&& msg)
    {
        m_pWalletNetwork->Send(peerID, std::move(msg));
    }

    void Wallet::OnWalletMessage(const WalletID& myID, wallet::SetTxParameter&& msg)
    {
        auto t = getTransaction(myID, msg);
        if (!t)
        {
            return;
        }
        bool txChanged = false;
        for (const auto& p : msg.m_Parameters)
        {
            if (p.first < TxParameterID::PrivateFirstParam)
            {
                txChanged |= t->SetParameter(p.first, p.second);
            }
            else
            {
                LOG_WARNING() << "Attempt to set private tx parameter";
            }
        }
        if (txChanged)
        {
            updateTransaction(msg.m_TxID);
        }
    }

    void Wallet::OnRequestComplete(MyRequestTransaction& r)
    {
        LOG_DEBUG() << r.m_TxID << (r.m_Res.m_Value ? " has registered" : " has failed to register");
        
        auto it = m_transactions.find(r.m_TxID);
        if (it != m_transactions.end())
        {
            it->second->SetParameter(TxParameterID::TransactionRegistered, r.m_Res.m_Value);
            updateTransaction(r.m_TxID);
        }
    }

    void Wallet::cancel_tx(const TxID& txId)
    {
        LOG_INFO() << "Canceling tx " << txId;

        if (auto it = m_transactions.find(txId); it != m_transactions.end())
        {
            it->second->Cancel();
        }
        else
        {
            m_WalletDB->deleteTx(txId);
        }
    }

    void Wallet::delete_tx(const TxID& txId)
    {
        LOG_INFO() << "deleting tx " << txId;
        if (auto it = m_transactions.find(txId); it == m_transactions.end())
        {
            m_WalletDB->deleteTx(txId);
        }
        else
        {
            LOG_WARNING() << "Cannot delete running transaction";
        }
    }

    void Wallet::updateTransaction(const TxID& txID)
    {
        auto it = m_transactions.find(txID);
        if (it != m_transactions.end())
        {
            bool bSynced = !SyncRemains();

            if (bSynced)
                it->second->Update();
            else
                m_TransactionsToUpdate.insert(it->second);
        }
        else
        {
            LOG_DEBUG() << txID << " Unexpected event";
        }
    }

    void Wallet::OnRequestComplete(MyRequestUtxo& r)
    {
        if (r.m_Res.m_Proofs.empty())
            return; // Right now nothing is concluded from empty proofs

        const auto& proof = r.m_Res.m_Proofs.front(); // Currently - no handling for multiple coins for the same commitment.

        Block::SystemState::Full sTip;
        get_tip(sTip);

        proto::UtxoEvent evt;
        evt.m_Added = 1;
        evt.m_Kidv = r.m_CoinID;
        evt.m_Maturity = proof.m_State.m_Maturity;
        evt.m_Height = sTip.m_Height;

        ProcessUtxoEvent(evt, sTip.m_Height); // uniform processing for all confirmed utxos
    }

    void Wallet::OnRequestComplete(MyRequestKernel& r)
    {
        if (!r.m_Res.m_Proof.empty())
        {
            m_WalletDB->get_History().AddStates(&r.m_Res.m_Proof.m_State, 1); // why not?

            auto it = m_transactions.find(r.m_TxID);
            if (m_transactions.end() != it)
            {
                if (it->second->SetParameter(TxParameterID::KernelProofHeight, r.m_Res.m_Proof.m_State.m_Height))
                    it->second->Update();
            }
        }
    }

    void Wallet::OnRequestComplete(MyRequestBbsMsg& r)
    {
        assert(false);
    }

    void Wallet::OnRequestComplete(MyRequestBbsChannel& r)
    {
        m_WalletDB->SetLastChannel(r.m_Res.m_Channel);
    }

    void Wallet::RequestUtxoEvents()
    {
        if (!m_OwnedNodesOnline)
            return;

        Block::SystemState::Full sTip;
        m_WalletDB->get_History().get_Tip(sTip);

        Height h = GetUtxoEventsHeightNext();
        assert(h <= sTip.m_Height + 1);
        if (h > sTip.m_Height)
            return;

        if (!m_PendingUtxoEvents.empty())
        {
            if (m_PendingUtxoEvents.begin()->m_Msg.m_HeightMin == h)
                return; // already pending
            DeleteReq(*m_PendingUtxoEvents.begin());
        }

        MyRequestUtxoEvents::Ptr pReq(new MyRequestUtxoEvents);
        pReq->m_Msg.m_HeightMin = h;
        PostReqUnique(*pReq);
    }

    void Wallet::AbortUtxoEvents()
    {
        if (!m_PendingUtxoEvents.empty())
            DeleteReq(*m_PendingUtxoEvents.begin());
    }

    void Wallet::OnRequestComplete(MyRequestUtxoEvents& r)
    {
        Block::SystemState::Full sTip;
        m_WalletDB->get_History().get_Tip(sTip);

        const std::vector<proto::UtxoEvent>& v = r.m_Res.m_Events;
		for (size_t i = 0; i < v.size(); i++)
		{
			const proto::UtxoEvent& evt = v[i];

			// filter-out false positives
			Scalar::Native sk;
			Point comm;
			m_WalletDB->calcCommitment(sk, comm, evt.m_Kidv);

			if (comm == evt.m_Commitment)
				ProcessUtxoEvent(evt, sTip.m_Height);
		}

        if (r.m_Res.m_Events.size() < proto::UtxoEvent::s_Max)
            SetUtxoEventsHeight(sTip.m_Height);
        else
        {
            SetUtxoEventsHeight(r.m_Res.m_Events.back().m_Height);
            RequestUtxoEvents(); // maybe more events pending
        }
    }

    void Wallet::SetUtxoEventsHeight(Height h)
    {
        uintBigFor<Height>::Type var;
        var = h + 1; // we're actually saving the next
        wallet::setVar(m_WalletDB, s_szNextUtxoEvt, var);
    }

    Height Wallet::GetUtxoEventsHeightNext()
    {
        uintBigFor<Height>::Type var;
        if (!wallet::getVar(m_WalletDB, s_szNextUtxoEvt, var))
            return 0;

        Height h;
        var.Export(h);
        return h;
    }

    void Wallet::ProcessUtxoEvent(const proto::UtxoEvent& evt, Height hTip)
    {
        Coin c;
        c.m_ID = evt.m_Kidv;

        bool bExists = m_WalletDB->find(c);

        const TxID* pTxID = NULL;


        LOG_INFO() << "CoinID: " << evt.m_Kidv << " Maturity=" << evt.m_Maturity << (evt.m_Added ? " Confirmed" : " Spent");

        if (evt.m_Added)
        {
            c.m_maturity = evt.m_Maturity;
            if (!c.m_confirmHeight || (c.m_confirmHeight > evt.m_Height)) // in case of std utxo proofs - the event height may be bigger than actual utxo height
                c.m_confirmHeight = evt.m_Height;
            c.m_status = (evt.m_Maturity <= hTip) ? Coin::Status::Available : Coin::Status::Maturing;

            if (c.m_createTxId)
                updateTransaction(*c.m_createTxId);
            pTxID = c.m_createTxId.get_ptr();

            if (!bExists)
                c.m_createHeight = evt.m_Height;
        }
        else
        {
            if (!bExists)
                return; // should alert!

            c.m_maturity = evt.m_Maturity;
            c.m_status = Coin::Status::Spent;
            pTxID = c.m_spentTxId.get_ptr();
        }

        m_WalletDB->save(c);

        if (!pTxID)
            return;

        auto it = m_transactions.find(*pTxID);
        if (it == m_transactions.end())
            return;

        Height h = 0;
        const auto& pTx = it->second;
        pTx->GetParameter(TxParameterID::KernelProofHeight, h);

        if (!h || (h > evt.m_Height))
        {
            h = evt.m_Height;
            pTx->SetParameter(TxParameterID::KernelProofHeight, h);
            m_TransactionsToUpdate.insert(pTx);
        }
    }

    void Wallet::OnRolledBack()
    {
        Block::SystemState::Full sTip;
        m_WalletDB->get_History().get_Tip(sTip);

        m_WalletDB->get_History().DeleteFrom(sTip.m_Height + 1);

        m_WalletDB->rollbackConfirmedUtxo(sTip.m_Height);

        for (auto it = m_transactions.begin(); m_transactions.end() != it; it++)
        {
            const auto& pTx = it->second;

            Height h;
            if (pTx->GetParameter(TxParameterID::KernelProofHeight, h) && (h > sTip.m_Height))
            {
                h = 0;
                pTx->SetParameter(TxParameterID::KernelProofHeight, h);
                m_TransactionsToUpdate.insert(pTx);
            }
        }

        Height h = GetUtxoEventsHeightNext();
        if (h > sTip.m_Height + 1)
            SetUtxoEventsHeight(sTip.m_Height);
    }

    void Wallet::OnNewTip()
    {
        m_WalletDB->ShrinkHistory();

        Block::SystemState::Full sTip;
        get_tip(sTip);
        if (!sTip.m_Height)
            return; //?!

        Block::SystemState::ID id, id2;
        sTip.get_ID(id);
        LOG_INFO() << "Sync up to " << id;

        if (!m_WalletDB->getSystemStateID(id2))
            id2.m_Height = 0;

        RequestUtxoEvents();

        auto t = m_transactions;
        for (auto& p : t)
        {
            p.second->Update();
        }

        // try to restore utxo state after reset, rollback and etc..
        uint32_t nUnconfirmed = 0;
        m_WalletDB->visit([&nUnconfirmed, this](const Coin& c)->bool
        {
            if (c.m_status == Coin::Unavailable
                && ((c.m_createTxId.is_initialized()
                && (m_transactions.find(*c.m_createTxId) == m_transactions.end())) || c.isReward()))
            {
                getUtxoProof(c.m_ID);
                nUnconfirmed++;
            }
            return true;
        });

        if (nUnconfirmed)
        {
            LOG_INFO() << "Found " << nUnconfirmed << " unconfirmed utxo to proof";
        }

        CheckSyncDone();

        if (m_PendingBbsChannel.empty())
        {
            BbsChannel ch;
            Timestamp t0 = m_WalletDB->GetLastChannel(ch);
            Timestamp t1 = getTimestamp();

            if (t0 + 3600 < t1)
            {
                MyRequestBbsChannel::Ptr pReq(new MyRequestBbsChannel);
                PostReqUnique(*pReq);
            }
        }
    }

    void Wallet::OnTipUnchanged()
    {
        LOG_INFO() << "Tip has not been changed";
        notifySyncProgress();
    }

    void Wallet::getUtxoProof(const Coin::ID& cid)
    {
        MyRequestUtxo::Ptr pReq(new MyRequestUtxo);
        pReq->m_CoinID = cid;

		Scalar::Native sk;
		m_WalletDB->calcCommitment(sk, pReq->m_Msg.m_Utxo, cid);

        LOG_DEBUG() << "Get utxo proof: " << pReq->m_Msg.m_Utxo;

        PostReqUnique(*pReq);
    }

    uint32_t Wallet::SyncRemains() const
    {
        size_t val =
#define THE_MACRO(type) m_Pending##type.size() +
            REQUEST_TYPES_Sync(THE_MACRO)
#undef THE_MACRO
            0;

        return static_cast<uint32_t>(val);
    }

    void Wallet::CheckSyncDone()
    {
        report_sync_progress();

        if (SyncRemains())
            return;

        m_LastSyncTotal = 0;

        saveKnownState();
    }

    void Wallet::saveKnownState()
    {
        Block::SystemState::Full sTip;
        get_tip(sTip);

        Block::SystemState::ID id;
        if (sTip.m_Height)
            sTip.get_ID(id);
        else
            ZeroObject(id);

        m_WalletDB->setSystemStateID(id);
        LOG_INFO() << "Current state is " << id;
        notifySyncProgress();

        std::unordered_set<wallet::BaseTransaction::Ptr> txSet;
        txSet.swap(m_TransactionsToUpdate);

        for (auto it = txSet.begin(); txSet.end() != it; it++)
        {
            const wallet::BaseTransaction::Ptr& pTx = *it;
            if (m_transactions.find(pTx->GetTxID()) != m_transactions.end())
                pTx->Update();
        }
    }

    void Wallet::notifySyncProgress()
    {
        uint32_t n = SyncRemains();
        for (auto sub : m_subscribers)
        {
            sub->onSyncProgress(m_LastSyncTotal - n, m_LastSyncTotal);
        }
    }

    void Wallet::report_sync_progress()
    {
        if (!m_LastSyncTotal)
            return;

        uint32_t nDone = m_LastSyncTotal - SyncRemains();
        assert(nDone <= m_LastSyncTotal);
        int p = static_cast<int>((nDone * 100) / m_LastSyncTotal);
        LOG_INFO() << "Synchronizing with node: " << p << "% (" << nDone << "/" << m_LastSyncTotal << ")";

        notifySyncProgress();
    }

    void Wallet::register_tx(const TxID& txId, Transaction::Ptr data)
    {
        LOG_VERBOSE() << txId << " sending tx for registration";

#ifndef NDEBUG
        TxBase::Context ctx;
        assert(data->IsValid(ctx));
#endif // NDEBUG

        MyRequestTransaction::Ptr pReq(new MyRequestTransaction);
        pReq->m_TxID = txId;
        pReq->m_Msg.m_Transaction = std::move(data);

        PostReqUnique(*pReq);
    }

    void Wallet::subscribe(IWalletObserver* observer)
    {
        assert(std::find(m_subscribers.begin(), m_subscribers.end(), observer) == m_subscribers.end());

        m_subscribers.push_back(observer);

        m_WalletDB->subscribe(observer);
    }

    void Wallet::unsubscribe(IWalletObserver* observer)
    {
        auto it = std::find(m_subscribers.begin(), m_subscribers.end(), observer);

        assert(it != m_subscribers.end());

        m_subscribers.erase(it);

        m_WalletDB->unsubscribe(observer);
    }

    wallet::BaseTransaction::Ptr Wallet::getTransaction(const WalletID& myID, const wallet::SetTxParameter& msg)
    {
        auto it = m_transactions.find(msg.m_TxID);
        if (it != m_transactions.end())
        {
            if (it->second->GetType() != msg.m_Type)
            {
                LOG_WARNING() << msg.m_TxID << " Parameters for invalid tx type";
            }
            return it->second;
        }

        TxType type = TxType::Simple;
        if (wallet::getTxParameter(m_WalletDB, msg.m_TxID, TxParameterID::TransactionType, type))
        {
            // we return only active transactions
            return BaseTransaction::Ptr();
        }

        auto t = constructTransaction(msg.m_TxID, msg.m_Type);

        t->SetParameter(TxParameterID::TransactionType, msg.m_Type, false);
        t->SetParameter(TxParameterID::CreateTime, getTimestamp(), false);
        t->SetParameter(TxParameterID::MyID, myID, false);
        t->SetParameter(TxParameterID::PeerID, msg.m_From, false);
        t->SetParameter(TxParameterID::IsInitiator, false, false);
        t->SetParameter(TxParameterID::Status, TxStatus::Pending, true);

        auto address = m_WalletDB->getAddress(myID);
        if (address.is_initialized())
        {
            ByteBuffer message(address->m_label.begin(), address->m_label.end());
            t->SetParameter(TxParameterID::Message, message);
        }

        m_transactions.emplace(msg.m_TxID, t);
        return t;
    }

    wallet::BaseTransaction::Ptr Wallet::constructTransaction(const TxID& id, TxType type)
    {
        switch (type)
        {
        case TxType::Simple:
             return make_shared<SimpleTransaction>(*this, m_WalletDB, id);
        case TxType::AtomicSwap:
            return make_shared<AtomicSwapTransaction>(*this, m_WalletDB, id);
        }
        return wallet::BaseTransaction::Ptr();
    }
}
