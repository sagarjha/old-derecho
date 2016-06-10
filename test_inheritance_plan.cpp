#include <SerializationSupport.hpp>
#include <queue>
#include <future>
#include <iostream>

namespace rpc{

	using id_t = unsigned long long;

	template<id_t, typename>
	struct RemoteInvocable;
	
	class LocalMessager{
	private:
		LocalMessager(){}
		using elem = std::pair<std::size_t, char const * const>;
		using queue_t = std::queue<elem>;
		
		std::shared_ptr<queue_t> _send;
		std::shared_ptr<queue_t> _receive;
		using l = std::unique_lock<std::mutex>;
		std::shared_ptr<std::mutex> m_send;
		std::shared_ptr<std::condition_variable> cv_send;
		std::shared_ptr<std::mutex> m_receive;
		std::shared_ptr<std::condition_variable> cv_receive;
		LocalMessager(decltype(_send) &_send,
					  decltype(_receive) &_receive,
					  decltype(m_send) &m_send,
					  decltype(cv_send) &cv_send,
					  decltype(m_receive) &m_receive,
					  decltype(cv_receive) &cv_receive)
			:_send(_send),
			 _receive(_receive),
			 m_send(m_send),
			 cv_send(cv_send),
			 m_receive(m_receive),
			 cv_receive(cv_receive){}
	public:
		LocalMessager(const LocalMessager&) = delete;
		LocalMessager(LocalMessager&&) = default;
		
		static std::pair<LocalMessager,LocalMessager> build_pair(){
			std::shared_ptr<queue_t> q1{new queue_t{}};
			std::shared_ptr<queue_t> q2{new queue_t{}};
			std::shared_ptr<std::mutex> m1{new std::mutex{}};
			std::shared_ptr<std::condition_variable> cv1{new std::condition_variable{}};
			std::shared_ptr<std::mutex> m2{new std::mutex{}};
			std::shared_ptr<std::condition_variable> cv2{new std::condition_variable{}};
			return std::pair<LocalMessager,LocalMessager>{LocalMessager{q1,q2,m1,cv1,m2,cv2},LocalMessager{q2,q1,m2,cv2,m1,cv1}};
		}
		
		void send(std::size_t s, char const * const v){
			l e{*m_send};
			cv_send->notify_all();
			_send->emplace(s,v);
		}
		
		elem receive(){
			l e{*m_receive};
			while (_receive->empty()){
				cv_receive->wait(e);
				}
			auto ret = _receive->front();
			_receive->pop();
			return ret;
		}
	};
	
	
	using receive_fun_t =
		std::function<std::pair<std::size_t, char *>
					  (mutils::DeserializationManager* dsm,
					   const char * recv_buf,
					   const std::function<char* (int)>& out_alloc)>;
	
	
//many versions of this class will be extended by a single Hanlders context.
//each specific instance of this class provies a mechanism for communicating with
//remote sites.
	template<id_t tag, typename Ret, typename... Args>
	struct RemoteInvocable<tag, Ret (Args...)> {
		
		using f_t = Ret (*) (Args...);
		const f_t f;
		static const id_t invoke_id;
		static const id_t reply_id;
		
		RemoteInvocable(std::map<id_t,receive_fun_t> &receivers, Ret (*f) (Args...)):f(f){
			receivers[invoke_id] = [this](auto... a){return receive_call(a...);};
			receivers[reply_id] = [this](auto... a){return receive_response(a...);};
		}
		
		std::queue<std::promise<Ret> > ret;
		std::mutex ret_lock;
		using lock_t = std::unique_lock<std::mutex>;

		//use this from within a derived class to receive precisely this RemoteInvocable
		//(this way, all RemoteInvocable methods do not need to worry about type collisions)
		inline RemoteInvocable& handler(std::integral_constant<id_t, tag> const * const, const Args & ...) {
			return *this;
		}

		using barray = char*;
		template<typename A>
		int to_bytes(barray& v, A && a){
			v += mutils::to_bytes(std::forward<A>(a), v);
			return 0;
		}
		
		std::tuple<int, char *, std::future<Ret> > Send(const std::function<char *(int)> &out_alloc,
														  Args && ...a){
			const auto size = (mutils::bytes_size(a) + ... + 0);
			auto *serialized_args = out_alloc(size);
			(void) (to_bytes(serialized_args,std::forward<Args>(a)) + ... + 0);
			lock_t l{ret_lock};
			ret.emplace();
			return std::make_tuple(size,serialized_args,ret.back().get_future());
		}
		
		using recv_ret = std::pair<std::size_t, char *>;
		
		inline recv_ret receive_response(mutils::DeserializationManager* dsm, const char* response, const std::function<char*(int)>&){
			lock_t l{ret_lock};
			ret.front().set_value(*mutils::from_bytes<Ret>(dsm,response));
			ret.pop();
			return recv_ret{0,nullptr};
		}

		
		template<typename _Type>
		inline auto deserialize(mutils::DeserializationManager *dsm, const char * mut_in){
			using Type = std::decay_t<_Type>;
			auto ds = mutils::from_bytes<Type>(dsm,mut_in);
			const auto size = mutils::bytes_size(*ds);
			mut_in += size;
			return ds;
		}
		

		inline recv_ret receive_call(std::false_type const * const, mutils::DeserializationManager* dsm,
							  const char * recv_buf,
							  const std::function<char* (int)>& out_alloc){
			const auto result = f(*deserialize<Args>(dsm,recv_buf)... );
			const auto result_size = mutils::bytes_size(result);
			auto out = out_alloc(result_size);
			mutils::to_bytes(result,out);
			return recv_ret{mutils::bytes_size(result),out};
		}
		
		inline recv_ret receive_call(std::true_type const * const, mutils::DeserializationManager* dsm,
							  const char * recv_buf,
							  const std::function<char* (int)>&){
			f(*deserialize<Args>(dsm,recv_buf)... );
			return recv_ret{0,nullptr};
		}

		inline recv_ret receive_call(mutils::DeserializationManager* dsm,
							  const char * recv_buf,
							  const std::function<char* (int)>& out_alloc){
			constexpr std::is_same<Ret,void> *choice{nullptr};
			return receive_call(choice, dsm, recv_buf, out_alloc);
		}
	};
	
	template<id_t tag, typename Ret, typename... Args>
	const id_t RemoteInvocable<tag, Ret (Args...)>::invoke_id{mutils::gensym()};

	template<id_t tag, typename Ret, typename... Args>
	const id_t RemoteInvocable<tag, Ret (Args...)>::reply_id{mutils::gensym()};
	
	template<typename...>
	struct RemoteInvocablePairs;
	
	template<>
	struct RemoteInvocablePairs<> {
		
	};
	
//id better be an integral constant of id_t
	template<id_t id, typename Q,typename... rest>
	struct RemoteInvocablePairs<std::integral_constant<id_t, id>, Q, rest...> :
		public RemoteInvocable<id,Q>,
		public RemoteInvocablePairs<rest...> {
		template<typename... T>
		RemoteInvocablePairs(std::map<id_t,receive_fun_t> &receivers, Q q, T && ... t)
			:RemoteInvocable<id,Q>(receivers, q),
			RemoteInvocablePairs<rest...>(std::forward<T>(t)...){}
	};
	
	template<typename... Fs>
	struct Handlers : private RemoteInvocablePairs<Fs...> {
	private:
		//point-to-point communication
		LocalMessager lm;
		bool alive{true};
		//constructed *before* initialization
		std::unique_ptr<std::map<id_t, receive_fun_t> > receivers;
		//constructed *after* initialization
		std::unique_ptr<std::thread> receiver;
		mutils::DeserializationManager dsm{{}};
		
		static char* extra_alloc (int i){
			return (char*) calloc(i + sizeof(id_t),sizeof(char)) + sizeof(id_t);
		}
		
	public:
		
		void receive_call_loop(){
			while (alive){
				auto recv_pair = lm.receive();
				auto *buf = recv_pair.second;
				auto size = recv_pair.first;
				assert(size);
				assert(size >= sizeof(id_t));
				assert(((id_t*)buf)[0]);
				auto reply_pair = receivers->at(((id_t*)buf)[0])(&dsm,buf + sizeof(id_t), extra_alloc);
				auto * reply_buf = reply_pair.second;
				if (reply_buf){
					reply_buf -= sizeof(id_t);
					((id_t*)reply_buf)[0] = reply_pair.first;
					lm.send(reply_pair.first,reply_buf);
				}
			}
		}

		//these are the functions (no names) from Fs
		template<typename... _Fs>
		Handlers(decltype(receivers) rvrs, LocalMessager _lm, _Fs... fs)
			:RemoteInvocablePairs<Fs...>(*rvrs,fs...),
			lm(std::move(_lm)),
			receivers(std::move(rvrs))
			{
				receiver.reset(new std::thread{[&](){receive_call_loop();}});
			}
		
		//these are the functions (no names) from Fs
		//delegation so receivers exists during superclass construction
		template<typename... _Fs>
		Handlers(LocalMessager _lm, _Fs... fs)
			:Handlers(std::make_unique<typename decltype(receivers)::element_type>(), std::move(_lm),fs...){}
		
		~Handlers(){
			alive = false;
			receiver->join();
		}
		
		template<id_t tag, typename... Args>
			auto Send(Args && ... args){
			constexpr std::integral_constant<id_t, tag>* choice{nullptr};
			auto &hndl = this->handler(choice,args...);
			auto sent_tuple = hndl.Send(extra_alloc,
										std::forward<Args>(args)...);
			std::size_t used = std::get<0>(sent_tuple);
			char * buf = std::get<1>(sent_tuple) - sizeof(id_t);
			((id_t*)buf)[0] = hndl.invoke_id;
			lm.send(used + sizeof(id_t),buf);
			return std::move(std::get<2>(sent_tuple));
		}
		
	};

	struct Handlers_erased{
		std::shared_ptr<void> erased_handlers;

		template<typename... T>
		Handlers_erased(std::unique_ptr<Handlers<T...> > h):erased_handlers(h.release()){}

		template<id_t tag, typename Ret, typename... Args>
		auto Send (Args && ... args){
			return static_cast<RemoteInvocable<tag, Ret (Args...) >* >(erased_handlers.get())->Send(std::forward<Args>(args)...);
		}
	};
}

using namespace rpc;

//handles up to 5 args
#define HANDLERS_TYPE_ARGS2(a, b) std::integral_constant<rpc::id_t, a>, decltype(b)
#define HANDLERS_TYPE_ARGS4(a, b,c...) std::integral_constant<rpc::id_t, a>, decltype(b), HANDLERS_TYPE_ARGS2(c)
#define HANDLERS_TYPE_ARGS6(a, b,c...) std::integral_constant<rpc::id_t, a>, decltype(b), HANDLERS_TYPE_ARGS4(c)
#define HANDLERS_TYPE_ARGS8(a, b,c...) std::integral_constant<rpc::id_t, a>, decltype(b), HANDLERS_TYPE_ARGS6(c)
#define HANDLERS_TYPE_ARGS10(a, b,c...) std::integral_constant<rpc::id_t, a>, decltype(b), HANDLERS_TYPE_ARGS8(c)
#define HANDLERS_TYPE_ARGS_IMPL2(count, ...) HANDLERS_TYPE_ARGS ## count (__VA_ARGS__)
#define HANDLERS_TYPE_ARGS_IMPL(count, ...) HANDLERS_TYPE_ARGS_IMPL2(count, __VA_ARGS__)
#define HANDLERS_TYPE_ARGS(...) HANDLERS_TYPE_ARGS_IMPL(VA_NARGS(__VA_ARGS__), __VA_ARGS__)

//handles up to 5 args
#define HANDLERS_ONLY_FUNS2(a, b) b
#define HANDLERS_ONLY_FUNS4(a, b,c...) b, HANDLERS_ONLY_FUNS2(c)
#define HANDLERS_ONLY_FUNS6(a, b,c...) b, HANDLERS_ONLY_FUNS4(c)
#define HANDLERS_ONLY_FUNS8(a, b,c...) b, HANDLERS_ONLY_FUNS6(c)
#define HANDLERS_ONLY_FUNS10(a, b,c...) b, HANDLERS_ONLY_FUNS8(c)
#define HANDLERS_ONLY_FUNS_IMPL2(count, ...) HANDLERS_ONLY_FUNS ## count (__VA_ARGS__)
#define HANDLERS_ONLY_FUNS_IMPL(count, ...) HANDLERS_ONLY_FUNS_IMPL2(count, __VA_ARGS__)
#define HANDLERS_ONLY_FUNS(...) HANDLERS_ONLY_FUNS_IMPL(VA_NARGS(__VA_ARGS__), __VA_ARGS__)

#define handlers(m,a...) std::make_unique<Handlers<HANDLERS_TYPE_ARGS(a)> >(m,HANDLERS_ONLY_FUNS(a))

int test1(int i){return i;}

int main() {
	auto msg_pair = LocalMessager::build_pair();
	auto hndlers1 = handlers(std::move(msg_pair.first),0,test1);
	auto hndlers2 = handlers(std::move(msg_pair.second),0,test1);
	assert(hndlers1->Send<0>(1).get() == 1);
	std::cout << "done" << std::endl;
}
