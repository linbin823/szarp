#include <zmq.hpp>
#include "szbase/szbbase.h"
#include "funtable.h"
float ChooseFun(float funid, float *parlst);
void putParamsFromString(const std::wstring& script_string, std::wregex& ipc_par_reg, const int& name_match_prefix, const int& name_match_sufix, std::vector<std::wstring>& ret_params);

namespace sz4 {
template <class time_type> time_type getTimeNow() { return *new time_type(NULL); }

template<> sz4::second_time_t getTimeNow<sz4::second_time_t>() {
	std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
	auto duration = now.time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
	return *new sz4::second_time_t(seconds);
}

template<> sz4::nanosecond_time_t getTimeNow<sz4::nanosecond_time_t>() {
	std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
	auto duration = now.time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
	const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
	return *new sz4::nanosecond_time_t(seconds, nanoseconds);
}
}

class DefParamBase: public sz4::param_observer {
public:
	DefParamBase(TParam *param, size_t index): param(param), index(index), preparedParams() {}

	virtual ~DefParamBase() {}

	virtual void setVal(double) = 0;
	bool shouldBeUpdated() const { return should_update; }
	virtual void setZmqVal(zmqhandler&) const = 0;
	virtual void setParcookVal(short*) const = 0;

	virtual void executeAndUpdate(zmqhandler&, short*) = 0;

	void param_data_changed(TParam *p) override { should_update = true; }

	virtual void prepareParamsFromScript() = 0;

	void subscribe_on_params(sz4::base* base) {
		std::vector<TParam*> params_to_register;

		const auto pushParamsToRegister = [&params_to_register](const std::wstring& name) {
			TParam* param_to_register = IPKContainer::GetObject()->GetParam(name);
			if (!param_to_register) {
				 // Something went wrong
				sz_log(0, "Error getting param %S to observed of LUA function", name.c_str());
				return;
			}
			if (std::find(params_to_register.begin(), params_to_register.end(), param_to_register) != params_to_register.end()) return;
			params_to_register.push_back(param_to_register);
			sz_log(6, "Added param %S to observed of LUA function in param s", param_to_register->GetGlobalName().c_str());
		};
		
		std::for_each(preparedParams.cbegin(), preparedParams.cend(), pushParamsToRegister);
		base->register_observer(this, params_to_register);
	}

protected:
	TParam *param;
	const size_t index;
	mutable bool should_update = true;
	std::vector<std::wstring> preparedParams;
};

template <class data_type, class time_type>
class BaseParamImpl: public DefParamBase {
public:
	BaseParamImpl(TParam* param, size_t index): DefParamBase(param, index), val(0), t() {}
	void setZmqVal(zmqhandler& zmq) const override {
		makeTimeNow();
		zmq.set_value(index, t, val);
		// this->should_update = false;
	}

	void setParcookVal(short* read) const override {
		data_type val_to_parcook = val;

		for (int i = this->param->GetPrec(); i > 0; i--) {
			val_to_parcook*= 10;
		}

		if (val_to_parcook > std::numeric_limits<short>::max() || val_to_parcook < std::numeric_limits<short>::min()) read[index] = SZARP_NO_DATA;
		else read[index] = (short) val_to_parcook;
	}

	void setVal(double d) override { // copy the value
		val = (data_type) d;
	}

private:
	data_type val;
	mutable time_type t;

	void makeTimeNow() const { t = sz4::getTimeNow<time_type>(); } // template time
};

template <class v, class t>
class LuaParam: public BaseParamImpl<v,t> {
	lua_State* lua;
public:
	LuaParam(TParam* param, size_t index, lua_State* lua): BaseParamImpl<v,t>(param, index), lua(lua) {
		compileScript();
	}

	void compileScript() {
		std::ostringstream paramfunction;

		paramfunction					<< 
		"return function()"			<< std::endl <<
		"	local i = param_value"		<< std::endl <<
		"	local p = szbase"			<< std::endl <<
		"	local state = {}"			<< std::endl <<
		"	local v = nil"				<< std::endl <<
		this->param->GetLuaScript()		<< std::endl <<
		"	return v"					<< std::endl <<
		"end"							<< std::endl;

		std::string str = paramfunction.str();
		const char* content = str.c_str();

		// Load string as LUA function named as the parameter
		int ret = luaL_loadbuffer(lua, content, strlen(content), (const char*)SC::S2U(this->param->GetName()).c_str());
		if (ret != 0) {
			throw SzException(std::string("Error compiling param ") + SC::S2A(this->param->GetName()) + lua_tostring(lua, -1));
		}

		// Test call
		ret = lua_pcall(lua, 0, 1, 0);
		if (ret != 0) {
			throw SzException(std::string("Error compiling param ") + SC::S2A(this->param->GetName()) + lua_tostring(lua, -1));
		}

		this->param->SetLuaParamRef(luaL_ref(lua, LUA_REGISTRYINDEX)); // register the lua function to call later
	}

	void prepareParamsFromScript() override {
		std::wstring lua_string(SC::U2S(this->param->GetLuaScript()));
		std::vector<std::wstring> ret_params;
		constexpr int name_match_prefix = 3;
		constexpr int name_match_sufix = 4;

		std::wregex ipc_par_regex(L"[ip]\\(\\\"([^:\\(\\)]*:){2}[^:\\(\\)]*\\\"");
		putParamsFromString(lua_string, ipc_par_regex, name_match_prefix, name_match_sufix, ret_params);
		std::wstring prefix = SC::A2S(std::string(libpar_getpar("", "prefix", 1))) + L":";
		prefix = prefix.substr(1, prefix.length());
		const auto addPrefix = [&prefix](std::wstring& name) {
			name.insert(0, prefix);
		};
		std::for_each(ret_params.begin(), ret_params.end(), addPrefix);

		ipc_par_regex.assign(L"[ip]\\(\\\"([^:\\(\\)]*:){3}[^:\\(\\)]*\\\"");
		putParamsFromString(lua_string, ipc_par_regex, name_match_prefix, name_match_sufix, ret_params);

		this->preparedParams = ret_params;
	}


	void executeAndUpdate(zmqhandler& zmq, short* read) override {
		if (!this->shouldBeUpdated()) { 
			this->setZmqVal(zmq);
			this->setParcookVal(read);

			return; 
		}

		assert(this->param->GetLuaParamReference() != LUA_NOREF);
		double result;

		// push param's lua function and call it
		lua_rawgeti(lua, LUA_REGISTRYINDEX, this->param->GetLuaParamReference());
		//lua_pushnumber(lua, time(NULL));
		//lua_pushnumber(lua, SZARP_PROBE_TYPE::PT_SEC10);
		int ret = lua_pcall(lua, 0, 1, 0);
		if (ret != 0) {
			throw SzException(std::string("Execution error for param ") + SC::S2A(this->param->GetName()) + lua_tostring(lua, -1));
		}

		if (lua_isnil(lua, -1))
			result = nan("");
		else 
			result = lua_tonumber(lua, -1);

		// pop the function
		lua_pop(lua, 1);

		// this->should_update=false;
		this->setVal(result);
		this->setZmqVal(zmq);
		this->setParcookVal(read);
	}

};

unsigned char CalculNoData;

template <class v, class t>
class RPNParam: public BaseParamImpl<v,t> {
public:
	std::wstring tab;
	RPNParam(TParam* param, size_t index, std::wstring tab): BaseParamImpl<v,t>(param, index), tab(tab) {}

	void prepareParamsFromScript() override {
		std::wstring rpn_string(this->param->GetFormula());
		std::vector<std::wstring> ret_params;
		constexpr int name_match_prefix = 1;
		constexpr int name_match_sufix = 2;

		std::wregex ipc_par_regex(L"\\(([^:\\(\\)]*:){2}[^:\\(\\)]*\\)");
		putParamsFromString(rpn_string, ipc_par_regex, name_match_prefix, name_match_sufix, ret_params);
		std::wstring prefix = SC::A2S(std::string(libpar_getpar("", "prefix", 1))) + L":";
		prefix = prefix.substr(1, prefix.length());
		const auto addPrefix = [&prefix](std::wstring& name) {
			name.insert(0, prefix);
		};
		std::for_each(ret_params.begin(), ret_params.end(), addPrefix);

		ipc_par_regex.assign(L"\\(([^:\\(\\)]*:){3}[^:\\(\\)]*\\)");
		putParamsFromString(rpn_string, ipc_par_regex, name_match_prefix, name_match_sufix, ret_params);
		this->preparedParams = ret_params;
	}

	void executeAndUpdate(zmqhandler& zmq, short* read) override
	{
		if (!this->shouldBeUpdated()) { 
			this->setZmqVal(zmq);
			this->setParcookVal(read);

			return; 
		}

		const wchar_t *chptr;
		constexpr int SS = 30;
		double stack[SS+1];
		char nodata[SS+1];
		double tmp;
		short sp = 0;
		short parcnt;
		int NullFormula = 0;
		int p_no = 0;
		float val_op = 0.0f; 
		this->setVal(sz4::no_data<v>());

		CalculNoData = 0;

		chptr = tab.c_str();
		do {
			if (sp >= SS) {
				sz_log(0, "parcook: stack overflow after %td chars when calculating formula '%ls'",
						chptr - tab.c_str(), tab.c_str());
				CalculNoData = 1;
				return;
			}
			if (iswdigit(*chptr)) {
				tmp = wcstof(chptr, NULL);
				chptr = wcschr(chptr, L' ');
				float par_val = Defdmn::IPCParamValue(this->preparedParams[p_no++]);
				nodata[sp] = 0; // check if val is not nan
				stack[sp++] = par_val;	
			} else {
				switch (*chptr) {
					case L'&':
						if (sp < 2)	/* swap */
							break;
						tmp = stack[sp - 1];
						stack[sp - 1] = stack[sp - 2];
						stack[sp - 2] = tmp;
						break;
					case L'!':
						stack[sp] = stack[sp - 1];	/* duplicate 
										 */
						sp++;
						break;
					case L'$':
						if (sp-- < 2)	/* function call */
							break;
						parcnt = (short) rint(stack[sp - 1]);
						if (sp < parcnt + 1)
							break;
						val_op = (float) stack[sp - parcnt - 1];
						stack[sp - parcnt - 1] =
							ChooseFun((float) stack[sp],
								  &val_op);
						sp -= parcnt;
						break;
					case L'#':
						nodata[sp] = 0;
						stack[sp++] = wcstof(++chptr, NULL);
						chptr = wcschr(chptr, L' ');
						break;
					case L'i':
						if (*(++chptr) == L'f') {	
					/* warunek <par1> <par2> <cond> if 
					 * jesli <cond> != 0 zostawia <par1> 
					 * w innym wypadku zostawia <par2> */
							if (sp-- < 3)	
								break;
							if (nodata[sp]) {
								CalculNoData = 1;
								break;
							}
							if (stack[sp] == 0)
								stack[sp - 2] =
									stack[sp - 1];
							sp--;
						}
						break;
					case L'n':
						chptr += 3;
						NullFormula = 1;
						break;
					case L'+':
						if (sp-- < 2)
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						stack[sp - 1] += stack[sp];
						break;
					case L'-':
						if (sp-- < 2)
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						stack[sp - 1] -= stack[sp];
						break;
					case L'*':
						if (sp-- < 2)
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						stack[sp - 1] *= stack[sp];
						break;
					case L'/':
						if (sp-- < 2)
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp] != 0.0)
							stack[sp - 1] /= stack[sp];
						else {
							stack[sp - 1] = 1;
							CalculNoData = 1;
						}
						break;
					case L'>':
						if (sp-- < 2)	/* wieksze */
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp - 1] > stack[sp])
							stack[sp - 1] = 1;
						else
							stack[sp - 1] = 0;
						break;
					case L'<':
						if (sp-- < 2)	/* mniejsze */
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp - 1] < stack[sp])
							stack[sp - 1] = 1;
						else
							stack[sp - 1] = 0;
						break;
					case L'~':
						if (sp-- < 2)	/* rowne */
							break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp - 1] == stack[sp])
							stack[sp - 1] = 1;
						else
							stack[sp - 1] = 0;
						break;
					case L'N' :	/* no-data */
						if (sp-- < 2) 
							break;
						if (nodata[sp - 1]) {
							stack[sp - 1] = stack[sp];
							nodata[sp - 1] = nodata[sp];
						}
						break;
					case L'm' :    /* no-data if stack[sp -2] < stack [sp - 1], otherwise
								stack[sp-2]	*/
						if (sp -- < 2) break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp-1] < stack[sp]) {
							nodata[sp-1] = 1;
						}
						break;
					case L'M' :    /* no-data if stack[sp -2] > stack [sp - 1], otherwise
								stack[sp-2]	*/
						if (sp -- < 2) break;
						if (nodata[sp] || nodata[sp-1]) {
							CalculNoData = 1;
							break;
						}
						if (stack[sp-1] > stack[sp]) {
							nodata[sp-1] = 1;
						}
						break;
					case L'=':
						if (sp-- < 2)
							break;
						if (CalculNoData)
							this->setVal(SZARP_NO_DATA);
						else if (NullFormula)
							break;
						else if (nodata[sp-1]) {
							this->setVal(SZARP_NO_DATA);
						} else 
							this->setVal(stack[sp - 1]);
						break;
					case L' ':
						break;
					default:
						sz_log(1, "Uknown character '%lc' in formula '%ls' for parameter",
								*chptr, this->tab.c_str());
				}
			}
		} while (*(++chptr) != 0);
			
		// this->should_update=false;
		this->setZmqVal(zmq);
		this->setParcookVal(read);
	}
};

class LuaParamBuilder
{
public:
	template<class data_type, class time_type> static DefParamBase* op(TParam* par, size_t index, lua_State* lua)
	{
		return new LuaParam<data_type, time_type>(par, index, lua);
	}
};

class RPNParamBuilder
{
public:
	template<class data_type, class time_type> static DefParamBase* op(TParam* par, size_t index, std::wstring tab)
	{
		return new RPNParam<data_type, time_type>(par, index, tab);
	}
};

/* vim: set filetype=cpp: */
