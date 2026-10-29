/**
 * @file llspellcheckengine_win32.cpp
 * @brief Spell-check engine backed by the native Windows Spell Checking API
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llstring.h"
#include "llthread.h"

#include "llspellcheckengine.h"

#include <objbase.h>
#include <spellcheck.h>

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // std::string (UTF-8) -> std::wstring (UTF-16). The default code page is CP_UTF8.
    std::wstring toWide(const std::string& str)
    {
        return ll_convert_string_to_wide(str.c_str(), str.length());
    }

    // A null-terminated wide string (e.g. an LPOLESTR returned by COM) -> UTF-8 std::string.
    std::string fromWide(const wchar_t* wstr)
    {
        return (wstr) ? ll_convert_wide_to_string(wstr, wcslen(wstr)) : std::string();
    }

    // <SK:Nexii> Run the Windows Spell Checking API on a worker thread so a stalled spelling service cannot freeze the viewer

    // How long the main thread waits for the worker on the calls whose answer it needs; past that
    // the call fails as if no checker were available, and the worker is left to finish.
    constexpr std::chrono::milliseconds LANGUAGE_TIMEOUT(2000);
    constexpr std::chrono::milliseconds SUGGEST_TIMEOUT(500);

    // A cache this large is dropped and refilled rather than grown further.
    constexpr size_t MAX_CACHED_WORDS = 16384;

    // The COM objects, touched only on the worker thread.
    struct WinSpellCom
    {
        ISpellCheckerFactory* mFactory = nullptr;
        ISpellChecker*        mChecker = nullptr;

        std::vector<std::string> availableTags() const;
        bool setLanguage(const std::string& name);
        bool checkWord(const std::wstring& word) const;
        void getSuggestions(const std::wstring& word, std::vector<std::string>& suggestions) const;
    };

    // What the main thread and the worker share, held by shared_ptr so that a worker stuck in a
    // call past the engine's destruction still owns what it touches.
    struct WinSpellShared
    {
        typedef std::function<void(WinSpellCom&)> task_t;

        std::mutex              mMutex;
        std::condition_variable mCondition;
        std::condition_variable mDoneCondition;
        std::deque<task_t>      mTasks;
        bool                    mStop = false;
        bool                    mDone = false;

        // Checked words awaiting the main thread; only those checked under mResultGeneration are kept.
        std::vector<std::pair<std::string, bool>> mResults;
        U32                                       mResultGeneration = 0;

        void post(task_t task)
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mTasks.push_back(std::move(task));
            }
            mCondition.notify_one();
        }
    };

    void runSpellWorker(std::shared_ptr<WinSpellShared> shared)
    {
        set_thread_name("SpellCheck");

        // Multithreaded apartment: the checker's calls block in place, with no message loop for this thread to pump.
        const HRESULT com_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        WinSpellCom com;
        if (SUCCEEDED(com_init))
        {
            if (FAILED(CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&com.mFactory))))
            {
                com.mFactory = nullptr;
            }
        }

        while (true)
        {
            WinSpellShared::task_t task;
            {
                std::unique_lock<std::mutex> lock(shared->mMutex);
                shared->mCondition.wait(lock, [&] { return shared->mStop || !shared->mTasks.empty(); });
                if (shared->mStop)
                {
                    break;
                }
                task = std::move(shared->mTasks.front());
                shared->mTasks.pop_front();
            }
            task(com);
        }

        // Release COM interfaces before tearing down the apartment.
        if (com.mChecker)
        {
            com.mChecker->Release();
        }
        if (com.mFactory)
        {
            com.mFactory->Release();
        }
        if (SUCCEEDED(com_init))
        {
            CoUninitialize();
        }

        {
            std::lock_guard<std::mutex> lock(shared->mMutex);
            shared->mDone = true;
        }
        shared->mDoneCondition.notify_all();
    }

    std::vector<std::string> WinSpellCom::availableTags() const
    {
        std::vector<std::string> tags;
        if (!mFactory)
        {
            return tags;
        }
        IEnumString* langs = nullptr;
        if (SUCCEEDED(mFactory->get_SupportedLanguages(&langs)) && langs)
        {
            LPOLESTR psz = nullptr;
            ULONG fetched = 0;
            while ((langs->Next(1, &psz, &fetched) == S_OK) && (1 == fetched) && psz)
            {
                tags.push_back(fromWide(psz));
                CoTaskMemFree(psz);
                psz = nullptr;
            }
            langs->Release();
        }
        return tags;
    }

    bool WinSpellCom::setLanguage(const std::string& name)
    {
        if (mChecker)
        {
            mChecker->Release();
            mChecker = nullptr;
        }
        if ( (!mFactory) || (name.empty()) )
        {
            return false;
        }

        const std::string tag = LLSpellCheckEngine::matchLanguage(name, availableTags(), '-');
        if (tag.empty())
        {
            return false;
        }

        const std::wstring wtag = toWide(tag);
        // CreateSpellChecker returns E_INVALIDARG when no checker is available for the language.
        if (FAILED(mFactory->CreateSpellChecker(wtag.c_str(), &mChecker)) || (!mChecker))
        {
            mChecker = nullptr;
            return false;
        }
        return true;
    }

    bool WinSpellCom::checkWord(const std::wstring& word) const
    {
        if (!mChecker)
        {
            return true;
        }

        IEnumSpellingError* errors = nullptr;
        if (FAILED(mChecker->Check(word.c_str(), &errors)) || (!errors))
        {
            return true;
        }

        // A correctly spelled word yields an empty enumeration: Next() returns S_FALSE. Any fetched
        // ISpellingError (S_OK) means the word is misspelled.
        ISpellingError* error = nullptr;
        const bool misspelled = (S_OK == errors->Next(&error)) && (nullptr != error);
        if (error)
        {
            error->Release();
        }
        errors->Release();

        return !misspelled;
    }

    void WinSpellCom::getSuggestions(const std::wstring& word, std::vector<std::string>& suggestions) const
    {
        if (!mChecker)
        {
            return;
        }

        IEnumString* enum_str = nullptr;
        const HRESULT hr = mChecker->Suggest(word.c_str(), &enum_str);
        if (FAILED(hr) || (!enum_str))
        {
            return;
        }

        // Suggest() returns S_FALSE when the word is already spelled correctly, in which case the
        // enumeration just echoes the input word - skip it so we don't offer the word as its own fix.
        if (S_OK == hr)
        {
            LPOLESTR psz = nullptr;
            ULONG fetched = 0;
            while ((enum_str->Next(1, &psz, &fetched) == S_OK) && (1 == fetched) && psz)
            {
                suggestions.push_back(fromWide(psz));
                CoTaskMemFree(psz);
                psz = nullptr;
            }
        }
        enum_str->Release();
    }

    // Every COM call runs on the worker: a word is checked in the background and reads as correct
    // until its answer arrives, and the calls whose answer the caller needs wait for it only so long.
    class LLWinSpellEngine final : public LLSpellCheckEngine
    {
    public:
        LLWinSpellEngine();
        ~LLWinSpellEngine() override;
        bool setLanguage(const std::string& name) override;
        bool isActive() const override { return mActive; }
        bool checkWord(const std::string& word) const override;
        bool takeCheckedWords() override;
        S32  getSuggestions(const std::string& word, std::vector<std::string>& suggestions) const override;
        std::set<std::string> getInstalledLanguages(const std::vector<std::string>& candidate_names) const override;
    private:
        // Run 'fn' on the worker and wait up to 'timeout' for its result, else return 'fallback'.
        template<typename T>
        T callWorker(std::function<T(WinSpellCom&)> fn, std::chrono::milliseconds timeout, T fallback) const;

        void collectResults() const;

        std::shared_ptr<WinSpellShared> mShared;
        bool                            mActive = false;
        U32                             mGeneration = 0;   // bumped by each language change

        mutable std::unordered_map<std::string, bool> mCache;     // word -> spelled correctly
        mutable std::unordered_set<std::string>       mPending;   // posted and not yet answered
        mutable bool                                  mChecked = false;   // answers came in since takeCheckedWords()
    };

    LLWinSpellEngine::LLWinSpellEngine()
        : mShared(std::make_shared<WinSpellShared>())
    {
        llassert(on_main_thread());
        std::thread(runSpellWorker, mShared).detach();
    }

    LLWinSpellEngine::~LLWinSpellEngine()
    {
        llassert(on_main_thread());

        // Ask the worker to release COM and stop; one stuck in a call is left behind rather than waited for.
        std::unique_lock<std::mutex> lock(mShared->mMutex);
        mShared->mStop = true;
        mShared->mTasks.clear();
        mShared->mCondition.notify_one();
        if (!mShared->mDoneCondition.wait_for(lock, LANGUAGE_TIMEOUT, [&] { return mShared->mDone; }))
        {
            LL_WARNS("SpellCheck") << "Spell check worker did not stop; leaving it behind" << LL_ENDL;
        }
    }

    template<typename T>
    T LLWinSpellEngine::callWorker(std::function<T(WinSpellCom&)> fn, std::chrono::milliseconds timeout, T fallback) const
    {
        auto promise = std::make_shared<std::promise<T>>();
        std::future<T> future = promise->get_future();
        mShared->post([fn = std::move(fn), promise](WinSpellCom& com) { promise->set_value(fn(com)); });
        if (future.wait_for(timeout) != std::future_status::ready)
        {
            LL_WARNS("SpellCheck") << "Spell check service did not answer in " << timeout.count() << " ms" << LL_ENDL;
            return fallback;
        }
        return future.get();
    }

    bool LLWinSpellEngine::setLanguage(const std::string& name)
    {
        llassert(on_main_thread());
        const U32 generation = ++mGeneration;
        mCache.clear();
        mPending.clear();
        {
            std::lock_guard<std::mutex> lock(mShared->mMutex);
            mShared->mResults.clear();
            mShared->mResultGeneration = generation;
        }

        mActive = callWorker<bool>([name](WinSpellCom& com) { return com.setLanguage(name); }, LANGUAGE_TIMEOUT, false);
        return mActive;
    }

    void LLWinSpellEngine::collectResults() const
    {
        std::vector<std::pair<std::string, bool>> results;
        {
            std::lock_guard<std::mutex> lock(mShared->mMutex);
            if (mShared->mResults.empty())
            {
                return;
            }
            results.swap(mShared->mResults);
        }
        if (mCache.size() + results.size() > MAX_CACHED_WORDS)
        {
            mCache.clear();
        }
        for (auto& result : results)
        {
            mPending.erase(result.first);
            mCache[std::move(result.first)] = result.second;
        }
        mChecked = true;
    }

    bool LLWinSpellEngine::takeCheckedWords()
    {
        llassert(on_main_thread());
        collectResults();
        const bool checked = mChecked;
        mChecked = false;
        return checked;
    }

    bool LLWinSpellEngine::checkWord(const std::string& word) const
    {
        llassert(on_main_thread());
        if (!mActive)
        {
            return true;
        }

        collectResults();
        const auto it = mCache.find(word);
        if (it != mCache.end())
        {
            return it->second;
        }

        std::wstring wword = toWide(word);
        if ( (!wword.empty()) && (mPending.insert(word).second) )
        {
            std::shared_ptr<WinSpellShared> shared = mShared;
            const U32 generation = mGeneration;
            mShared->post([shared, generation, word, wword = std::move(wword)](WinSpellCom& com)
                {
                    const bool correct = com.checkWord(wword);
                    std::lock_guard<std::mutex> lock(shared->mMutex);
                    if (shared->mResultGeneration == generation)
                    {
                        shared->mResults.emplace_back(word, correct);
                    }
                });
        }
        return true;
    }

    S32 LLWinSpellEngine::getSuggestions(const std::string& word, std::vector<std::string>& suggestions) const
    {
        llassert(on_main_thread());
        suggestions.clear();
        if (!mActive)
        {
            return 0;
        }

        const std::wstring wword = toWide(word);
        if (wword.empty())
        {
            return 0;
        }

        suggestions = callWorker<std::vector<std::string>>([wword](WinSpellCom& com)
            {
                std::vector<std::string> found;
                com.getSuggestions(wword, found);
                return found;
            }, SUGGEST_TIMEOUT, {});
        return static_cast<S32>(suggestions.size());
    }

    std::set<std::string> LLWinSpellEngine::getInstalledLanguages(const std::vector<std::string>& candidate_names) const
    {
        llassert(on_main_thread());
        // One OS query for the whole batch.
        const std::vector<std::string> available = callWorker<std::vector<std::string>>(
            [](WinSpellCom& com) { return com.availableTags(); }, LANGUAGE_TIMEOUT, {});

        std::set<std::string> installed;
        for (const std::string& name : candidate_names)
        {
            if (!LLSpellCheckEngine::matchLanguage(name, available, '-').empty())
            {
                installed.insert(name);
            }
        }
        return installed;
    }
    // </SK:Nexii>
}

// static
std::unique_ptr<LLSpellCheckEngine> LLSpellCheckEngine::create()
{
    return std::make_unique<LLWinSpellEngine>();
}
