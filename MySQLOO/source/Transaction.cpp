#include "Transaction.h"
#include "ResultData.h"
#include "errmsg.h"
#include "Database.h"

Transaction::Transaction(Database* dbase, GarrysMod::Lua::ILuaBase* LUA) : IQuery(dbase, LUA) {
	registerFunction(LUA, "addQuery", Transaction::addQuery);
	registerFunction(LUA, "clearQueries", Transaction::clearQueries);
}

void Transaction::onDestroyed(GarrysMod::Lua::ILuaBase* LUA) {}

//TODO Fix memory leak if transaction is never started
int Transaction::addQuery(lua_State* state) {
	GarrysMod::Lua::ILuaBase* LUA = state->luabase;
	LUA->SetState(state);
	Transaction* transaction = dynamic_cast<Transaction*>(unpackSelf(LUA, TYPE_QUERY));
	if (transaction == nullptr) {
		LUA->ThrowError("Tried to pass wrong self");
	}
	IQuery* iQuery = (IQuery*)unpackLuaObject(LUA, 2, TYPE_QUERY, false);
	Query* query = dynamic_cast<Query*>(iQuery);
	if (query == nullptr) {
		LUA->ThrowError("Tried to pass non query to addQuery()");
	}
	auto queryPtr = std::dynamic_pointer_cast<Query>(iQuery->getSharedPointerInstance());
	auto queryData = iQuery->buildQueryData(LUA);
	if (iQuery->runningQueryData.size() == 0) {
		iQuery->referenceTable(LUA, iQuery, 2);
	}
	std::shared_ptr<IQueryData> ptr = iQuery->buildQueryData(LUA);
	iQuery->addQueryData(LUA, queryData);
	transaction->m_queries.push_back(std::make_pair(queryPtr, queryData));
	return 0;
}

int Transaction::clearQueries(lua_State* state) {
	GarrysMod::Lua::ILuaBase* LUA = state->luabase;
	LUA->SetState(state);
	Transaction* transaction = dynamic_cast<Transaction*>(unpackSelf(LUA, TYPE_QUERY));
	if (transaction == nullptr) {
		LUA->ThrowError("Tried to pass wrong self");
	}

	transaction->m_queries.clear();
	return 0;
}

//Calls the lua callbacks associated with this query
void Transaction::doCallback(GarrysMod::Lua::ILuaBase* LUA, std::shared_ptr<IQueryData> ptr) {
	TransactionData* data = (TransactionData*)ptr.get();
	data->setStatus(QUERY_COMPLETE);
	switch (data->getResultStatus()) {
	case QUERY_NONE:
		break;
	case QUERY_ERROR:
		if (data->getErrorReference() != 0) {
			this->runFunction(LUA, data->getErrorReference(), "s", data->getError().c_str());
		} else if (data->isFirstData()) {
			this->runCallback(LUA, "onError", "s", data->getError().c_str());
		}
		break;
	case QUERY_SUCCESS:
		if (data->getSuccessReference() != 0) {
			this->runFunction(LUA, data->getSuccessReference());
		} else if (data->isFirstData()) {
			this->runCallback(LUA, "onSuccess");
		}
		break;
	}
}

bool Transaction::executeStatement(MYSQL* connection, std::shared_ptr<IQueryData> ptr) {
	TransactionData* data = (TransactionData*)ptr.get();
	data->setStatus(QUERY_RUNNING);
	//This temporarily disables reconnect, since a reconnect
	//would rollback (and cancel) a transaction
	//Which could lead to parts of the transaction being executed outside of a transaction
	//If they are being executed after the reconnect
	my_bool oldReconnectStatus = m_database->getAutoReconnect();
	m_database->setAutoReconnect((my_bool)0);
	auto resetReconnectStatus = finally([&] { m_database->setAutoReconnect(oldReconnectStatus); });
	try {
		this->mysqlAutocommit(connection, false);
		{
			for (auto& query : data->m_queries) {
				auto curquery = query.first.get();
				auto &curdata = query.second;
				//Errors are cleared in case this is retrying after losing connection
				curdata->setResultStatus(QUERY_NONE);
				curdata->setError("");
				try {
					curquery->executeQuery(connection, curdata);
					curdata->setResultStatus(QUERY_SUCCESS);
					curdata->setFinished(true);
				} catch (const MySQLException& error) {
					curdata->setResultStatus(QUERY_ERROR);
					curdata->setError(error.what());
					curdata->setFinished(true);
					throw error;
				}
			}
		}
		if (mysql_commit(connection)) {
			throw MySQLException(CR_UNKNOWN_ERROR, "commit failed");
		}
		data->setResultStatus(QUERY_SUCCESS);
	} catch (const MySQLException& error) {
		data->setResultStatus(QUERY_ERROR);
		data->setError(error.what());

		int errorCode = error.getErrorCode();
		if (oldReconnectStatus && !data->retried &&
			(errorCode == CR_SERVER_LOST || errorCode == CR_SERVER_GONE_ERROR)) {
			//Because autoreconnect is disabled we want to try and explicitly execute the transaction once more
			//if we can get the client to reconnect (reconnect is caused by mysql_ping)
			//If this fails we just go ahead and error
			m_database->setAutoReconnect((my_bool)1);
			if (mysql_ping(connection) == 0) {
				for (auto& query : data->m_queries) {
					query.second->setFinished(false);
				}
				data->retried = true;
				return executeStatement(connection, ptr);
			}
		}
		//If this call fails it means that the connection was (probably) lost
		//In that case the mysql server rolls back any transaction anyways so it doesn't
		//matter if it fails
		mysql_rollback(connection);
	}

	for (auto& query : data->m_queries) {
		auto curquery = query.first.get();
		auto &curdata = query.second;
		if (!curdata->isFinished())
			continue;

		m_database->finishedQueries.put(query);
		{
			std::unique_lock<std::mutex> queryMutex(curquery->m_waitMutex);
			curquery->m_waitWakeupVariable.notify_one();
		}
	}

	//If this fails it probably means that the connection was lost
	//In that case autocommit is turned back on anyways (once the connection is reestablished)
	//See: https://dev.mysql.com/doc/refman/5.7/en/auto-reconnect.html
	mysql_autocommit(connection, true);

	data->setStatus(QUERY_COMPLETE);
	return true;
}


std::shared_ptr<IQueryData> Transaction::buildQueryData(GarrysMod::Lua::ILuaBase* LUA) {
	//At this point the transaction is guaranteed to have a referenced table
	//since this is always called shortly after transaction:start()
	std::shared_ptr<IQueryData> ptr(new TransactionData());
	TransactionData* data = (TransactionData*)ptr.get();
	data->m_queries = this->m_queries;
	this->m_queries.clear();
	return ptr;
}