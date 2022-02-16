// Copyright (c) 2018 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_TRANSACTION_BUILDER_H
#define ZCASH_TRANSACTION_BUILDER_H

#include "coins.h"
#include "consensus/params.h"
#include "keystore.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "script/standard.h"
#include "uint256.h"
#include "zcash/Address.hpp"
#include "zcash/IncrementalMerkleTree.hpp"
#include "zcash/JoinSplit.hpp"
#include "zcash/Note.hpp"
#include "zcash/NoteEncryption.hpp"

#include <optional>

#include <rust/builder.h>

#define NO_MEMO {{0xF6}}

namespace orchard { class UnauthorizedBundle; }

uint256 ProduceZip244SignatureHash(
    const CTransaction& tx,
    const orchard::UnauthorizedBundle& orchardBundle);

namespace orchard {

/// A builder that constructs an `UnauthorizedBundle` from a set of notes to be spent,
/// and recipients to receive funds.
class Builder {
private:
    /// The Orchard builder. Memory is allocated by Rust. If this is `nullptr` then
    /// `Builder::Build` has been called, and all subsequent operations will throw an
    /// exception.
    std::unique_ptr<OrchardBuilderPtr, decltype(&orchard_builder_free)> inner;

    Builder() : inner(nullptr, orchard_builder_free) { }

public:
    Builder(bool spendsEnabled, bool outputsEnabled, uint256 anchor);

    // Builder should never be copied
    Builder(const Builder&) = delete;
    Builder& operator=(const Builder&) = delete;
    Builder(Builder&& builder) : inner(std::move(builder.inner)) {}
    Builder& operator=(Builder&& builder)
    {
        if (this != &builder) {
            inner = std::move(builder.inner);
        }
        return *this;
    }

    /// Adds an address which will receive funds in this bundle.
    void AddOutput(
        const std::optional<uint256>& ovk,
        const libzcash::OrchardRawAddress& to,
        CAmount value,
        const std::optional<std::array<unsigned char, ZC_MEMO_SIZE>>& memo);

    /// Builds a bundle containing the given spent notes and recipients.
    ///
    /// Returns `std::nullopt` if an error occurs.
    ///
    /// Calling this method invalidates this object; in particular, if an error occurs
    /// this builder must be discarded and a new builder created. Subsequent usage of this
    /// object in any way will cause an exception. This emulates Rust's compile-time move
    /// semantics at runtime.
    std::optional<UnauthorizedBundle> Build();
};

/// An unauthorized Orchard bundle, ready for its proof to be created and signatures
/// applied.
class UnauthorizedBundle {
private:
    /// An optional Orchard bundle (with `nullptr` corresponding to `None`).
    /// Memory is allocated by Rust.
    std::unique_ptr<OrchardUnauthorizedBundlePtr, decltype(&orchard_unauthorized_bundle_free)> inner;

    UnauthorizedBundle() : inner(nullptr, orchard_unauthorized_bundle_free) {}
    UnauthorizedBundle(OrchardUnauthorizedBundlePtr* bundle) : inner(bundle, orchard_unauthorized_bundle_free) {}
    friend class Builder;
    // The parentheses here are necessary to avoid the following compilation error:
    //     error: C++ requires a type specifier for all declarations
    //             friend uint256 ::ProduceZip244SignatureHash(
    //             ~~~~~~           ^
    friend uint256 (::ProduceZip244SignatureHash(
        const CTransaction& tx,
        const UnauthorizedBundle& orchardBundle));

public:
    // UnauthorizedBundle should never be copied
    UnauthorizedBundle(const UnauthorizedBundle&) = delete;
    UnauthorizedBundle& operator=(const UnauthorizedBundle&) = delete;
    UnauthorizedBundle(UnauthorizedBundle&& bundle) : inner(std::move(bundle.inner)) {}
    UnauthorizedBundle& operator=(UnauthorizedBundle&& bundle)
    {
        if (this != &bundle) {
            inner = std::move(bundle.inner);
        }
        return *this;
    }

    /// Adds proofs and signatures to this bundle.
    ///
    /// Returns `std::nullopt` if an error occurs.
    ///
    /// Calling this method invalidates this object; in particular, if an error occurs
    /// this bundle must be discarded and a new bundle built. Subsequent usage of this
    /// object in any way will cause an exception. This emulates Rust's compile-time
    /// move semantics at runtime.
    std::optional<OrchardBundle> ProveAndSign(uint256 sighash);
};

} // namespace orchard

struct SpendDescriptionInfo {
    libzcash::SaplingExpandedSpendingKey expsk;
    libzcash::SaplingNote note;
    uint256 alpha;
    uint256 anchor;
    SaplingWitness witness;

    SpendDescriptionInfo(
        libzcash::SaplingExpandedSpendingKey expsk,
        libzcash::SaplingNote note,
        uint256 anchor,
        SaplingWitness witness);
};

struct OutputDescriptionInfo {
    uint256 ovk;
    libzcash::SaplingNote note;
    std::array<unsigned char, ZC_MEMO_SIZE> memo;

    OutputDescriptionInfo(
        uint256 ovk,
        libzcash::SaplingNote note,
        std::array<unsigned char, ZC_MEMO_SIZE> memo) : ovk(ovk), note(note), memo(memo) {}

    std::optional<OutputDescription> Build(void* ctx);
};

struct JSDescriptionInfo {
    Ed25519VerificationKey joinSplitPubKey;
    uint256 anchor;
    // We store references to these so they are correctly randomised for the caller.
    std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS>& inputs;
    std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS>& outputs;
    CAmount vpub_old;
    CAmount vpub_new;

    JSDescriptionInfo(
        Ed25519VerificationKey joinSplitPubKey,
        uint256 anchor,
        std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS>& inputs,
        std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS>& outputs,
        CAmount vpub_old,
        CAmount vpub_new) : joinSplitPubKey(joinSplitPubKey), anchor(anchor), inputs(inputs), outputs(outputs), vpub_old(vpub_old), vpub_new(vpub_new) {}

    JSDescription BuildDeterministic(
        bool computeProof = true, // Set to false in some tests
        uint256* esk = nullptr    // payment disclosure
    );

    JSDescription BuildRandomized(
        std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
        std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap,
        bool computeProof = true, // Set to false in some tests
        uint256* esk = nullptr,   // payment disclosure
        std::function<int(int)> gen = GetRandInt
    );
};

struct TransparentInputInfo {
    CScript scriptPubKey;
    CAmount value;

    TransparentInputInfo(
        CScript scriptPubKey,
        CAmount value) : scriptPubKey(scriptPubKey), value(value) {}
};

class TransactionBuilderResult {
private:
    std::optional<CTransaction> maybeTx;
    std::optional<std::string> maybeError;
public:
    TransactionBuilderResult() = delete;
    TransactionBuilderResult(const CTransaction& tx);
    TransactionBuilderResult(const std::string& error);
    bool IsTx();
    bool IsError();
    CTransaction GetTxOrThrow();
    std::string GetError();
};

class TransactionBuilder
{
private:
    Consensus::Params consensusParams;
    int nHeight;
    const CKeyStore* keystore;
    const CCoinsViewCache* coinsView;
    CCriticalSection* cs_coinsView;
    CMutableTransaction mtx;
    CAmount fee = 10000;
    std::optional<orchard::Builder> orchardBuilder;
    CAmount valueBalanceOrchard = 0;

    std::vector<SpendDescriptionInfo> spends;
    std::vector<OutputDescriptionInfo> outputs;
    std::vector<libzcash::JSInput> jsInputs;
    std::vector<libzcash::JSOutput> jsOutputs;
    std::vector<TransparentInputInfo> tIns;

    std::optional<std::pair<uint256, libzcash::SaplingPaymentAddress>> saplingChangeAddr;
    std::optional<libzcash::SproutPaymentAddress> sproutChangeAddr;
    std::optional<CTxDestination> tChangeAddr;

public:
    TransactionBuilder() {}
    TransactionBuilder(
        const Consensus::Params& consensusParams,
        int nHeight,
        std::optional<uint256> orchardAnchor,
        CKeyStore* keyStore = nullptr,
        CCoinsViewCache* coinsView = nullptr,
        CCriticalSection* cs_coinsView = nullptr);

    // TransactionBuilder should never be copied
    TransactionBuilder(const TransactionBuilder&) = delete;
    TransactionBuilder& operator=(const TransactionBuilder&) = delete;
    TransactionBuilder(TransactionBuilder&& builder) :
        consensusParams(std::move(builder.consensusParams)),
        nHeight(std::move(builder.nHeight)),
        keystore(std::move(builder.keystore)),
        coinsView(std::move(builder.coinsView)),
        cs_coinsView(std::move(builder.cs_coinsView)),
        mtx(std::move(builder.mtx)),
        fee(std::move(builder.fee)),
        orchardBuilder(std::move(builder.orchardBuilder)),
        valueBalanceOrchard(std::move(builder.valueBalanceOrchard)),
        spends(std::move(builder.spends)),
        outputs(std::move(builder.outputs)),
        jsInputs(std::move(builder.jsInputs)),
        jsOutputs(std::move(builder.jsOutputs)),
        tIns(std::move(builder.tIns)),
        saplingChangeAddr(std::move(builder.saplingChangeAddr)),
        sproutChangeAddr(std::move(builder.sproutChangeAddr)),
        tChangeAddr(std::move(builder.tChangeAddr)) {}
    TransactionBuilder& operator=(TransactionBuilder&& builder)
    {
        if (this != &builder) {
            consensusParams = std::move(builder.consensusParams);
            nHeight = std::move(builder.nHeight);
            keystore = std::move(builder.keystore);
            coinsView = std::move(builder.coinsView);
            cs_coinsView = std::move(builder.cs_coinsView);
            mtx = std::move(builder.mtx);
            fee = std::move(builder.fee);
            orchardBuilder = std::move(builder.orchardBuilder);
            valueBalanceOrchard = std::move(builder.valueBalanceOrchard);
            spends = std::move(builder.spends);
            outputs = std::move(builder.outputs);
            jsInputs = std::move(builder.jsInputs);
            jsOutputs = std::move(builder.jsOutputs);
            tIns = std::move(builder.tIns);
            saplingChangeAddr = std::move(builder.saplingChangeAddr);
            sproutChangeAddr = std::move(builder.sproutChangeAddr);
            tChangeAddr = std::move(builder.tChangeAddr);
        }
        return *this;
    }

    void SetExpiryHeight(uint32_t nExpiryHeight);

    void SetFee(CAmount fee);

    void AddOrchardOutput(
        const std::optional<uint256>& ovk,
        const libzcash::OrchardRawAddress& to,
        CAmount value,
        const std::optional<std::array<unsigned char, ZC_MEMO_SIZE>>& memo);

    // Throws if the anchor does not match the anchor used by
    // previously-added Sapling spends.
    void AddSaplingSpend(
        libzcash::SaplingExpandedSpendingKey expsk,
        libzcash::SaplingNote note,
        uint256 anchor,
        SaplingWitness witness);

    void AddSaplingOutput(
        uint256 ovk,
        libzcash::SaplingPaymentAddress to,
        CAmount value,
        std::array<unsigned char, ZC_MEMO_SIZE> memo = NO_MEMO);

    // Throws if the anchor does not match the anchor used by
    // previously-added Sprout inputs.
    void AddSproutInput(
        libzcash::SproutSpendingKey sk,
        libzcash::SproutNote note,
        SproutWitness witness);

    void AddSproutOutput(
        libzcash::SproutPaymentAddress to,
        CAmount value,
        std::array<unsigned char, ZC_MEMO_SIZE> memo = NO_MEMO);

    // Assumes that the value correctly corresponds to the provided UTXO.
    void AddTransparentInput(COutPoint utxo, CScript scriptPubKey, CAmount value);

    void AddTransparentOutput(const CTxDestination& to, CAmount value);

    void SendChangeTo(const libzcash::RecipientAddress& changeAddr, const uint256& ovk);
    void SendChangeToSprout(const libzcash::SproutPaymentAddress& changeAddr);

    TransactionBuilderResult Build();

private:
    void CheckOrSetUsingSprout();

    void CreateJSDescriptions();

    void CreateJSDescription(
        uint64_t vpub_old,
        uint64_t vpub_new,
        std::array<libzcash::JSInput, ZC_NUM_JS_INPUTS> vjsin,
        std::array<libzcash::JSOutput, ZC_NUM_JS_OUTPUTS> vjsout,
        std::array<size_t, ZC_NUM_JS_INPUTS>& inputMap,
        std::array<size_t, ZC_NUM_JS_OUTPUTS>& outputMap);
};

#endif // ZCASH_TRANSACTION_BUILDER_H
