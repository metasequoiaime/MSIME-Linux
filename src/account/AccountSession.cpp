#include "AccountSession.h"
#include <chrono>
#include <utility>

namespace metasequoia::linux_ime::account
{
AccountSession::AccountSession(BackendAccountClient &client, AccountCredentialStore &store, Clock clock)
    : client_(client), store_(store), clock_(std::move(clock))
{
    if (!clock_)
    {
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
    }
}
SessionSnapshot AccountSession::current() const
{
    return {generation_, session_ ? std::optional<User>(session_->tokens.user) : std::nullopt};
}
SessionSnapshot AccountSession::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return current();
}
SessionSnapshot AccountSession::restore()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!restored_)
    {
        // A failed lookup can be retried; it must not become a signed-out state.
        auto restored = store_.load();
        session_ = std::move(restored);
        restored_ = true;
        ++generation_;
    }
    return current();
}
void AccountSession::require_generation(std::uint64_t generation) const
{
    if (!restored_ || generation != generation_)
    {
        throw Failure(0, true);
    }
}
SavedSession AccountSession::saved(Tokens tokens) const
{
    const auto now = clock_();
    if (now <= 0 || now > 253402214399LL)
    {
        throw Failure(0);
    }
    const auto expiry = now + tokens.expires_in;
    return {std::move(tokens), expiry};
}
SessionSnapshot AccountSession::login(std::uint64_t generation, const std::string &challenge,
                                      const std::string &credential, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    require_generation(generation);
    auto next = saved(client_.login(challenge, credential, {}, cancelled));
    store_.save(next);
    session_ = std::move(next);
    ++generation_;
    return current();
}
Challenge AccountSession::begin_link(std::uint64_t generation, const std::string &provider, const std::string &target,
                                     const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return client_.challenge(provider, target, authorized(generation, cancelled), cancelled);
}
SessionSnapshot AccountSession::link(std::uint64_t generation, const std::string &challenge,
                                     const std::string &credential, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto token = authorized(generation, cancelled);
    auto next = saved(client_.login(challenge, credential, token, cancelled));
    if (next.tokens.user.id != session_->tokens.user.id)
        throw Failure(0);
    store_.save(next);
    session_ = std::move(next);
    ++generation_;
    return current();
}
void AccountSession::discard()
{
    // Server revocation already succeeded. Never keep using the revoked token,
    // even if the keyring is locked and clearing it reports a failure.
    session_.reset();
    ++generation_;
    store_.clear();
}
std::string AccountSession::authorized(std::uint64_t generation, const online::CancellationCheck &cancelled)
{
    require_generation(generation);
    if (cancelled && cancelled())
    {
        throw Failure(0, true);
    }
    if (!session_)
    {
        throw Failure(401);
    }
    const auto now = clock_();
    if (now <= 0 || now > 253402214399LL)
    {
        throw Failure(0);
    }
    if (session_->expires_at <= now + 60)
    {
        Tokens refreshed;
        try
        {
            refreshed = client_.refresh(session_->tokens.refresh_token, cancelled);
        }
        catch (const Failure &error)
        {
            if (error.status() == 401)
            {
                discard();
            }
            throw;
        }
        if (refreshed.user.id != session_->tokens.user.id)
        {
            throw Failure(0);
        }
        auto next = saved(std::move(refreshed));
        store_.save(next);
        session_ = std::move(next);
    }
    return session_->tokens.access_token;
}
std::string AccountSession::access_token(std::uint64_t generation, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return authorized(generation, cancelled);
}
Profile AccountSession::read_profile(const std::string &token, const online::CancellationCheck &cancelled)
{
    Profile profile;
    try
    {
        profile = client_.profile(token, cancelled);
    }
    catch (const Failure &error)
    {
        if (error.status() == 401)
            discard();
        throw;
    }
    if (profile.user.id != session_->tokens.user.id)
        throw Failure(0);
    auto next = *session_;
    next.tokens.user = profile.user;
    store_.save(next);
    session_ = std::move(next);
    return profile;
}
Profile AccountSession::profile(std::uint64_t generation, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return read_profile(authorized(generation, cancelled), cancelled);
}
Profile AccountSession::rename(std::uint64_t generation, const std::string &name,
                               const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto token = authorized(generation, cancelled);
    client_.rename(name, token, cancelled);
    return read_profile(token, cancelled);
}
void AccountSession::logout(std::uint64_t generation, bool all, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto token = authorized(generation, cancelled);
    client_.logout(token, all, cancelled);
    discard();
}
void AccountSession::delete_account(std::uint64_t generation, const online::CancellationCheck &cancelled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto token = authorized(generation, cancelled);
    client_.delete_account(token, cancelled);
    discard();
}
} // namespace metasequoia::linux_ime::account
