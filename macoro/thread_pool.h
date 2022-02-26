#pragma once
#include <vector>
#include <thread>
#include <mutex>
#include <deque>
#include <unordered_set>

#include "macoro/coroutine_handle.h"
#include "macoro/awaiter.h"
#include "cancellation.h"
#include <algorithm>
namespace macoro
{
	namespace detail
	{
		using thread_pool_clock = std::chrono::steady_clock;
		using thread_pool_time_point = std::chrono::time_point<thread_pool_clock>;
		struct thread_pool_delay_op
		{
			thread_pool_delay_op() = default;
			thread_pool_delay_op(const thread_pool_delay_op&) = default;
			thread_pool_delay_op(thread_pool_delay_op&&) = default;
			thread_pool_delay_op& operator=(const thread_pool_delay_op&) = default;
			thread_pool_delay_op& operator=(thread_pool_delay_op&&) = default;


			thread_pool_delay_op(std::size_t i, coroutine_handle<> h, thread_pool_time_point p)
				: idx(i)
				, handle(h)
				, deadline(p)
			{}

			std::size_t idx;
			coroutine_handle<> handle;
			thread_pool_time_point deadline;

			bool operator<(const thread_pool_delay_op& o) const { return deadline > o.deadline; }
		};



		struct thread_pool_state
		{
			std::mutex              mMutex;
			std::condition_variable mCondition;

			std::size_t mWork = 0;
			static thread_local thread_pool_state* mCurrentExecutor;

			std::deque<coroutine_handle<void>> mDeque;


			std::vector<thread_pool_delay_op> mDelayHeap;
			std::size_t mDelayOpIdx = 0;
			std::vector<std::thread> mThreads;

			void post(coroutine_handle<void> fn)
			{
				assert(fn);
				{
					std::lock_guard<std::mutex> lock(mMutex);
					mDeque.push_back(std::move(fn));
				}
				mCondition.notify_one();
			}

			MACORO_NODISCARD
				bool try_dispatch(coroutine_handle<void> fn)
			{
				if (mCurrentExecutor == this)
					return true;
				else
				{
					{
						std::lock_guard<std::mutex> lock(mMutex);
						mDeque.push_back(std::move(fn));
					}

					mCondition.notify_one();
					return false;
				}
			}

			void post_after(
				coroutine_handle<> h,
				thread_pool_time_point deadline,
				cancellation_token&& token,
				optional<cancellation_registration>& reg)
			{

				{
					std::unique_lock<std::mutex> lock(mMutex);
					auto idx = mDelayOpIdx++;
					mDelayHeap.emplace_back(idx, h, deadline);
					std::push_heap(mDelayHeap.begin(), mDelayHeap.end());

					if (token.can_be_cancelled())
					{
						reg.emplace(token, [idx, this] {
							cancel_delay_op(idx);
							});
					}
				}
				mCondition.notify_one();
			}


			void cancel_delay_op(std::size_t idx)
			{
				bool notify = false;
				{
					std::unique_lock<std::mutex> lock(mMutex);
					for (std::size_t i = 0; i < mDelayHeap.size(); ++i)
					{
						if (mDelayHeap[i].idx == idx)
						{
							mDelayHeap[i].deadline = thread_pool_clock::now();
							std::make_heap(mDelayHeap.begin(), mDelayHeap.end());
							notify = true;
							break;
						}
					}
				}
				if (notify)
					mCondition.notify_one();
			}

		};


		struct thread_pool_post
		{
			thread_pool_state* mPool;

			bool await_ready() const noexcept { return false; }

			template<typename H>
			void await_suspend(H h) const { 
				mPool->post(coroutine_handle<void>(h)); }

			void await_resume() const noexcept {}
		};


		struct thread_pool_dispatch
		{
			detail::thread_pool_state* pool;

			bool await_ready() const noexcept { return false; }

			template<typename H>
			auto await_suspend(const H& h)const {
				using traits_C = coroutine_handle_traits<H>;
				using return_type = typename traits_C::template coroutine_handle<void>;
				if (pool->try_dispatch(coroutine_handle<void>(h)))
					return return_type(h);
				else
					return static_cast<return_type>(noop_coroutine());
			}

			void await_resume() const noexcept {}
		};

		struct thread_pool_post_after
		{
			thread_pool_state* mPool;
			thread_pool_time_point mDeadline;
			cancellation_token mToken;
			optional<cancellation_registration> mReg;


			bool await_ready() const noexcept { return false; }

			template<typename H>
			void await_suspend(const H& h) {
				using traits_C = coroutine_handle_traits<H>;
				using return_type = typename traits_C::template coroutine_handle<void>;
				mPool->post_after(coroutine_handle<void>(h), mDeadline, std::move(mToken), mReg);
			}

			void await_resume() const noexcept {}
		};
	}

	class thread_pool
	{
	public:
		using clock = detail::thread_pool_clock;

		thread_pool()
			:mState(new detail::thread_pool_state)
		{}
		thread_pool(thread_pool&&) = default;
		thread_pool& operator=(thread_pool&&) = default;

		~thread_pool()
		{
			join();
		}

		std::unique_ptr<detail::thread_pool_state> mState;

		struct work
		{
			detail::thread_pool_state* mEx = nullptr;

			work(detail::thread_pool_state* e)
				: mEx(e)
			{
				std::lock_guard<std::mutex> lock(mEx->mMutex);
				++mEx->mWork;
			}

			work() = default;
			work(const work&) = delete;
			work(work&& w) : mEx(std::exchange(w.mEx, nullptr)) {}
			work& operator=(const work&) = delete;
			work& operator=(work&& w) { 
				reset();
				mEx = w.mEx; 
				return *this; 
			}

			void reset()
			{

				if (mEx)
				{

					std::size_t v = 0;
					{
						std::lock_guard<std::mutex> lock(mEx->mMutex);
						v = --mEx->mWork;
					}
					if (v == 0)
					{
						mEx->mCondition.notify_all();
					}
					mEx = nullptr;
				}
			}
			~work()
			{
				reset();
			}
		};

		detail::thread_pool_post schedule()
		{
			return { mState.get() };
		}


		detail::thread_pool_post post()
		{
			return { mState.get() };
		}


		detail::thread_pool_dispatch dispatch()
		{
			return { mState.get() };
		}

		void post(coroutine_handle<void> fn)
		{
			mState->post(fn);
		};

		void dispatch(coroutine_handle<void> fn)
		{
			if (mState->try_dispatch(fn))
				fn.resume();
		};

		template<typename Rep, typename Per>
		detail::thread_pool_post_after schedule_after(
			std::chrono::duration<Rep, Per> delay,
			cancellation_token token = {})
		{
			return { mState.get(), delay + clock::now(), std::move(token) };
		}

		work make_work() {
			return { mState.get() };
		}

		void create_threads(std::size_t n)
		{
			std::unique_lock<std::mutex> lock(mState->mMutex);
			if (mState->mWork || mState->mDeque.size() || mState->mDelayHeap.size())
			{
				mState->mThreads.reserve(mState->mThreads.size() + n);
				for (std::size_t i = 0; i < n; ++i)
				{
					mState->mThreads.emplace_back([this] {run(); });
				}
			}
		}

		void create_thread()
		{
			create_threads(1);
		}

		void join()
		{
			std::vector<std::thread> thrds;
			{
				std::unique_lock<std::mutex> lock(mState->mMutex);
				thrds = std::move(mState->mThreads);
			}

			for (auto& t : thrds)
				t.join();
		}

		void run()
		{
			auto state = mState.get();

			if (detail::thread_pool_state::mCurrentExecutor != nullptr)
				throw std::runtime_error("calling run() on a thread that is already controlled by a thread_pool is not supported. ");
			detail::thread_pool_state::mCurrentExecutor = state;

			coroutine_handle<void> fn;

			{
				std::unique_lock<std::mutex> lock(state->mMutex);
				while (
					state->mWork ||
					state->mDeque.size() ||
					state->mDelayHeap.size())
				{

					if ((state->mDelayHeap.empty() || state->mDelayHeap.front().deadline > clock::now()) &&
						state->mDeque.empty())
					{
						if (state->mDelayHeap.size())
						{
							state->mCondition.wait_until(lock, state->mDelayHeap.front().deadline);
						}
						else
						{
							state->mCondition.wait(lock, [&] {
								// wake up when theres something in the
								//  queue, or state->state->mWork == 0
								return
									state->mDeque.size() ||
									state->mDelayHeap.size() ||
									state->mWork == 0;
								});
						}
					}

					if (state->mDelayHeap.size() && state->mDelayHeap.front().deadline <= clock::now())
					{
						fn = state->mDelayHeap.front().handle;
						std::pop_heap(state->mDelayHeap.begin(), state->mDelayHeap.end());
						state->mDelayHeap.pop_back();
					}
					else if (state->mDeque.size())
					{
						fn = state->mDeque.front();
						state->mDeque.pop_front();
					}

					if (fn)
					{
						lock.unlock();

						fn.resume();
						fn = {};

						lock.lock();
					}
				}
			}

			detail::thread_pool_state::mCurrentExecutor = nullptr;
		}


	};

}