#include "when_all_tests.h"
#include "macoro/when_all.h"
#include "macoro/task.h"
#include "macoro/sync_wait.h"

namespace macoro
{

	void when_all_basic_tests()
	{
		auto f = []() -> task<int> {
			co_return 42;
		};


		auto g = []() -> task<bool> {
			co_return true;
		};

		bool b;
		auto h = [&]() -> task<bool&> {
			co_return b;
		};

		static_assert(
			is_awaitable<
			task<int>
			>::value
			);

		static_assert(std::is_same_v<
			remove_reference_and_wrapper_t<task<int>>,
			task<int>
		>);


		static_assert(
			is_awaitable<
			remove_reference_and_wrapper_t<task<int>>
			>::value
			);

		static_assert(
			std::conjunction<
			is_awaitable<
			remove_reference_and_wrapper_t<task<int>>
			>,
			is_awaitable<
			remove_reference_and_wrapper_t<task<bool>>
			>
			>::value
			);

		std::tuple <
			impl::when_all_task<int&&>,
			impl::when_all_task<bool&&>
			>
			r = sync_wait(when_all_ready(f(), g()));

		auto ff = f();
		auto gg = g();

		std::tuple <
			impl::when_all_task<int&&>,
			impl::when_all_task<bool&&>
		>
			r2 = sync_wait(when_all_ready(std::move(ff), h()));
		//auto r = sync_wait(w);
		impl::when_all_task<int&&> r0 = std::move(std::get<0>(r));
		assert(std::get<0>(r).result() == 42);
		assert(std::get<1>(r).result() == true);
	}

	void when_all_tests()
	{
	}
}
