#include <iostream>
#include "../storage.h"
#include "../navigator.h"
#include "../../utility/serialize.h"

#include "../ecc_native.h"

namespace ECC {
	// not really used, it's just the stupid linker
	Context g_Ctx;
	const Context& Context::get() { return g_Ctx; }
}

#ifndef WIN32
#	include <unistd.h>
#endif // WIN32

int g_TestsFailed = 0;

void TestFailed(const char* szExpr, uint32_t nLine)
{
	printf("Test failed! Line=%u, Expression: %s\n", nLine, szExpr);
	g_TestsFailed++;
}

#define verify_test(x) \
	do { \
		if (!(x)) \
			TestFailed(#x, __LINE__); \
	} while (false)

namespace beam
{
	class BlockChainClient
		:public ChainNavigator
	{
		struct Type {
			enum Enum {
				MyPatch = ChainNavigator::Type::count,
				count
			};
		};

	public:

		struct Header
			:public ChainNavigator::FixedHdr
		{
			uint32_t m_pDatas[30];
		};


		struct PatchPlus
			:public Patch
		{
			uint32_t m_iIdx;
			int32_t m_Delta;
		};

		void assert_valid() const { ChainNavigator::assert_valid(); }

		void Commit(uint32_t iIdx, int32_t nDelta)
		{
			PatchPlus* p = (PatchPlus*) m_Mapping.Allocate(Type::MyPatch, sizeof(PatchPlus));
			p->m_iIdx = iIdx;
			p->m_Delta = nDelta;

			ChainNavigator::Commit(*p);

			assert_valid();
		}

		void Tag(uint8_t n)
		{
			TagInfo ti;
			ZeroObject(ti);

			ti.m_Tag.m_pData[0] = n;
			ti.m_Difficulty = 1;
			ti.m_Height = 1;

			CreateTag(ti);

			assert_valid();
		}

	protected:
		// ChainNavigator
		virtual void AdjustDefs(MappedFile::Defs&d)
		{
			d.m_nBanks = Type::count;
			d.m_nFixedHdr = sizeof(Header);
		}

		virtual void Delete(Patch& p)
		{
			m_Mapping.Free(Type::MyPatch, &p);
		}

		virtual void Apply(const Patch& p, bool bFwd)
		{
			PatchPlus& pp = (PatchPlus&) p;
			Header& hdr = (Header&) get_Hdr_();

			verify_test(pp.m_iIdx < _countof(hdr.m_pDatas));

			if (bFwd)
				hdr.m_pDatas[pp.m_iIdx] += pp.m_Delta;
			else
				hdr.m_pDatas[pp.m_iIdx] -= pp.m_Delta;
		}

		virtual Patch* Clone(Offset x)
		{
			// during allocation ptr may change
			PatchPlus* pRet = (PatchPlus*) m_Mapping.Allocate(Type::MyPatch, sizeof(PatchPlus));
			PatchPlus& src = (PatchPlus&) get_Patch_(x);

			*pRet = src;

			return pRet;
		}

		virtual void assert_valid(bool b)
		{
			verify_test(b);
		}
	};


void DeleteFile(const char* szPath)
{
#ifdef WIN32
	DeleteFileA(szPath);
#else // WIN32
	unlink(szPath);
#endif // WIN32
}

	void TestNavigator()
	{
#ifdef WIN32
		const char* sz = "mytest.bin";
#else // WIN32
		const char* sz = "/tmp/mytest.bin";
#endif // WIN32

		DeleteFile(sz);

		BlockChainClient bcc;

		bcc.Open(sz);
		bcc.assert_valid();

		bcc.Tag(15);

		bcc.Commit(0, 15);
		bcc.Commit(3, 10);

		bcc.MoveBwd();
		bcc.assert_valid();

		bcc.Tag(76);

		bcc.Commit(9, 35);
		bcc.Commit(10, 20);

		bcc.MoveBwd();
		bcc.assert_valid();

		for (ChainNavigator::Offset x = bcc.get_ChildTag(); x; x = bcc.get_NextTag(x))
		{
			bcc.MoveFwd(x);
			bcc.assert_valid();

			bcc.MoveBwd();
			bcc.assert_valid();
		}

		bcc.MoveFwd(bcc.get_ChildTag());
		bcc.assert_valid();

		bcc.Close();
		bcc.Open(sz);
		bcc.assert_valid();

		bcc.Tag(44);
		bcc.Commit(12, -3);

		bcc.MoveBwd();
		bcc.assert_valid();

		bcc.DeleteTag(bcc.get_Hdr().m_TagCursor); // will also move bkwd
		bcc.assert_valid();

		for (ChainNavigator::Offset x = bcc.get_ChildTag(); x; x = bcc.get_NextTag(x))
		{
			bcc.MoveFwd(x);
			bcc.assert_valid();

			bcc.MoveBwd();
			bcc.assert_valid();
		}
	}

	void SetRandomUtxoKey(UtxoID& id)
	{
		for (int i = 0; i < sizeof(id.m_Commitment.m_X.m_pData); i++)
			id.m_Commitment.m_X.m_pData[i] = (uint8_t) rand();

		id.m_Commitment.m_Y	= (1 & rand()) != 0;
		id.m_Confidential		= (1 & rand()) != 0;
		id.m_Coinbase			= (1 & rand()) != 0;

		for (int i = 0; i < sizeof(id.m_Height); i++)
			((uint8_t*) &id.m_Height)[i] = (uint8_t) rand();
	}

	void TestUtxoTree()
	{
		std::vector<UtxoTree::Key> vKeys;
		vKeys.resize(70000);

		UtxoTree t;
		Merkle::Hash hv1, hv2, hvMid;

		for (uint32_t i = 0; i < vKeys.size(); i++)
		{
			UtxoTree::Key& key = vKeys[i];

			// random key
			UtxoID id0, id1;
			SetRandomUtxoKey(id0);

			key = id0;
			key.ToID(id1);

			verify_test(id0 == id1);

			UtxoTree::Cursor cu;
			bool bCreate = true;
			UtxoTree::MyLeaf* p = t.Find(cu, key, bCreate);

			verify_test(p && bCreate);
			p->m_Value.m_Count = i;

			if (!(i % 17))
			{
				t.get_Hash(hv1); // try to confuse clean/dirty

				for (int k = 0; k < 10; k++)
				{
					uint32_t j = rand() % (i + 1);

					bCreate = false;
					p = t.Find(cu, vKeys[j], bCreate);
					assert(p && !bCreate);

					Merkle::Proof proof;
					t.get_Proof(proof, cu);

					Merkle::Hash hvElement;
					p->m_Value.get_Hash(hvElement, p->m_Key);

					Merkle::Interpret(hvElement, proof);
					verify_test(hvElement == hv1);
				}
			}
		}

		t.get_Hash(hv1);

		for (uint32_t i = 0; i < vKeys.size(); i++)
		{
			if (i == vKeys.size()/2)
				t.get_Hash(hvMid);

			UtxoTree::Cursor cu;
			bool bCreate = true;
			UtxoTree::MyLeaf* p = t.Find(cu, vKeys[i], bCreate);

			verify_test(p && !bCreate);
			verify_test(p->m_Value.m_Count == i);
			t.Delete(cu);

			if (!(i % 31))
				t.get_Hash(hv2); // try to confuse clean/dirty
		}

		t.get_Hash(hv2);
		verify_test(hv2 == ECC::Zero);

		// construct tree in different order
		for (uint32_t i = (uint32_t) vKeys.size(); i--; )
		{
			const UtxoTree::Key& key = vKeys[i];

			UtxoTree::Cursor cu;
			bool bCreate = true;
			UtxoTree::MyLeaf* p = t.Find(cu, key, bCreate);

			verify_test(p && bCreate);
			p->m_Value.m_Count = i;

			if (!(i % 11))
				t.get_Hash(hv2); // try to confuse clean/dirty

			if (i == vKeys.size()/2)
			{
				t.get_Hash(hv2);
				verify_test(hv2 == hvMid);
			}
		}

		t.get_Hash(hv2);
		verify_test(hv2 == hv1);

		verify_test(vKeys.size() == t.Count());

		// serialization
		Serializer ser;
		t.save(ser);

		SerializeBuffer sb = ser.buffer();

		Deserializer der;
		der.reset(sb.first, sb.second);

		t.load(der);

		t.get_Hash(hv2);
		verify_test(hv2 == hv1);
	}

	struct MyMmr
		:public Merkle::Mmr
	{
		typedef std::vector<Merkle::Hash> HashVector;
		typedef std::unique_ptr<HashVector> HashVectorPtr;

		std::vector<HashVectorPtr> m_vec;

		Merkle::Hash& get_At(uint64_t nIdx, uint8_t nHeight)
		{
			if (m_vec.size() <= nHeight)
				m_vec.resize(nHeight + 1);

			HashVectorPtr& ptr = m_vec[nHeight];
			if (!ptr)
				ptr.reset(new HashVector);

		
			HashVector& vec = *ptr;
			if (vec.size() <= size_t(nIdx))
				vec.resize(size_t(nIdx) + 1);

			return vec[size_t(nIdx)];
		}

		virtual void LoadElement(Merkle::Hash& hv, uint64_t nIdx, uint8_t nHeight) const override
		{
			hv = ((MyMmr*) this)->get_At(nIdx, nHeight);
		}

		virtual void SaveElement(const Merkle::Hash& hv, uint64_t nIdx, uint8_t nHeight) override
		{
			get_At(nIdx, nHeight) = hv;
		}
	};

	struct MyDmmr
		:public Merkle::DistributedMmr
	{
		struct Node
		{
			typedef std::unique_ptr<Node> Ptr;

			Merkle::Hash m_MyHash;
			std::unique_ptr<uint8_t[]> m_pArr;
		};

		std::vector<Node::Ptr> m_AllNodes;

		virtual const void* get_NodeData(Key key) const override
		{
			assert(key);
			return ((Node*) key)->m_pArr.get();
		}

		virtual void get_NodeHash(Merkle::Hash& hash, Key key) const override
		{
			hash = ((Node*) key)->m_MyHash;
		}

		void MyAppend(const Merkle::Hash& hv)
		{
			uint32_t n = get_NodeSize(m_Count);

			MyDmmr::Node::Ptr p(new MyDmmr::Node);
			p->m_MyHash = hv;

			if (n)
				p->m_pArr.reset(new uint8_t[n]);

			Append((Key) p.get(), p->m_pArr.get(), p->m_MyHash);
			m_AllNodes.push_back(std::move(p));
		}
	};

	void TestMmr()
	{
		std::vector<Merkle::Hash> vHashes;
		vHashes.resize(300);

		MyMmr mmr;
		MyDmmr dmmr;
		Merkle::CompactMmr cmmr;

		for (uint32_t i = 0; i < vHashes.size(); i++)
		{
			Merkle::Hash& hv = vHashes[i];

			for (int i = 0; i < sizeof(hv.m_pData); i++)
				hv.m_pData[i] = (uint8_t)rand();

			Merkle::Hash hvRoot, hvRoot2, hvRoot3;

			mmr.get_PredictedHash(hvRoot, hv);
			dmmr.get_PredictedHash(hvRoot2, hv);
			cmmr.get_PredictedHash(hvRoot3, hv);
			verify_test(hvRoot == hvRoot2);
			verify_test(hvRoot == hvRoot3);

			mmr.Append(hv);
			dmmr.MyAppend(hv);
			cmmr.Append(hv);

			mmr.get_Hash(hvRoot);
			verify_test(hvRoot == hvRoot3);
			dmmr.get_Hash(hvRoot);
			verify_test(hvRoot == hvRoot3);
			cmmr.get_Hash(hvRoot);
			verify_test(hvRoot == hvRoot3);

			for (uint32_t j = 0; j <= i; j++)
			{
				Merkle::Proof proof, proof2;
				mmr.get_Proof(proof, j);
				dmmr.get_Proof(proof2, j);

				verify_test(proof == proof2);

				Merkle::Hash hv2 = vHashes[j];
				Merkle::Interpret(hv2, proof);
				verify_test(hv2 == hvRoot);
			}
		}
	}

} // namespace beam

int main()
{
	beam::TestNavigator();
	beam::TestUtxoTree();
	beam::TestMmr();

	return g_TestsFailed ? -1 : 0;
}
