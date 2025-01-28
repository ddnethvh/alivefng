#include "connection.h"

#include <sqlite3.h>
#include <base/system.h>
#include <engine/console.h>
#include <algorithm>

#include <atomic>

class CSqliteConnection : public IDbConnection
{
public:
	CSqliteConnection(const char *pFilename, bool Setup) :
		m_pDb(nullptr),
		m_pStmt(nullptr),
		m_Setup(Setup),
		m_Done(true)
	{
		str_copy(m_aFilename, pFilename, sizeof(m_aFilename));
	}

	~CSqliteConnection() override
	{
		if(m_pStmt)
			sqlite3_finalize(m_pStmt);
		if(m_pDb)
			sqlite3_close(m_pDb);
	}

	bool Connect(char *pError, int ErrorSize) override
	{
		if(m_pDb)
			return false;

		// Create storage directory
		char aPath[512];
		fs_storage_path("", aPath, sizeof(aPath));
		fs_makedir(aPath);

		// Open database
		int Result = sqlite3_open(m_aFilename, &m_pDb);
		if(Result != SQLITE_OK)
		{
			str_format(pError, ErrorSize, "Can't open sqlite database: %s", sqlite3_errmsg(m_pDb));
			if(m_pDb)
			{
				sqlite3_close(m_pDb);
				m_pDb = nullptr;
			}
			return true;
		}

		// Configure database
		Execute("PRAGMA journal_mode = DELETE", pError, ErrorSize);
		Execute("PRAGMA synchronous = FULL", pError, ErrorSize);
		Execute("PRAGMA cache_size = 1000", pError, ErrorSize);
		
		sqlite3_busy_timeout(m_pDb, 500);  // 500ms timeout

		return false;
	}

	void Disconnect() override
	{
		if(m_pStmt)
		{
			sqlite3_finalize(m_pStmt);
			m_pStmt = nullptr;
		}
	}

	bool PrepareStatement(const char *pStmt, char *pError, int ErrorSize) override
	{
		if(m_pStmt)
			sqlite3_finalize(m_pStmt);
		m_pStmt = nullptr;

		int Result = sqlite3_prepare_v2(m_pDb, pStmt, -1, &m_pStmt, nullptr);
		if(Result != SQLITE_OK)
		{
			str_copy(pError, sqlite3_errmsg(m_pDb), ErrorSize);
			return true;
		}
		m_Done = false;
		return false;
	}

	void BindString(int Idx, const char *pString) override
	{
		sqlite3_bind_text(m_pStmt, Idx, pString, -1, nullptr);
		m_Done = false;
	}

	void BindInt(int Idx, int Value) override
	{
		sqlite3_bind_int(m_pStmt, Idx, Value);
		m_Done = false;
	}

	bool Step(bool *pEnd, char *pError, int ErrorSize) override
	{
		if(m_Done)
		{
			*pEnd = true;
			return false;
		}

		int Result = sqlite3_step(m_pStmt);
		if(Result == SQLITE_ROW)
		{
			*pEnd = false;
			return false;
		}
		else if(Result == SQLITE_DONE)
		{
			m_Done = true;
			*pEnd = true;
			return false;
		}
		
		str_copy(pError, sqlite3_errmsg(m_pDb), ErrorSize);
		*pEnd = true;
		return true;
	}

	bool ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize) override
	{
		bool End;
		if(Step(&End, pError, ErrorSize))
			return true;
		*pNumUpdated = sqlite3_changes(m_pDb);
		return false;
	}

	bool IsNull(int Col) override
	{
		return sqlite3_column_type(m_pStmt, Col - 1) == SQLITE_NULL;
	}

	int GetInt(int Col) override
	{
		return sqlite3_column_int(m_pStmt, Col - 1);
	}

	void GetString(int Col, char *pBuffer, int BufferSize) override
	{
		str_copy(pBuffer, (const char *)sqlite3_column_text(m_pStmt, Col - 1), BufferSize);
	}

	bool AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize) override
	{
		const char *pQuery = 
			"INSERT INTO fng_ratings(Name, Rating) "
			"VALUES (?, ?) "
			"ON CONFLICT(Name) DO UPDATE SET Rating=Rating+?";

		if(PrepareStatement(pQuery, pError, ErrorSize))
			return true;
		BindString(1, pPlayer);
		BindInt(2, Points);
		BindInt(3, Points);
		bool End;
		return Step(&End, pError, ErrorSize);
	}

private:
	bool Execute(const char *pQuery, char *pError, int ErrorSize)
	{
		char *pErrorMsg;
		int Result = sqlite3_exec(m_pDb, pQuery, nullptr, nullptr, &pErrorMsg);
		if(Result != SQLITE_OK)
		{
			str_format(pError, ErrorSize, "Error executing query: %s", pErrorMsg);
			sqlite3_free(pErrorMsg);
			return true;
		}
		return false;
	}

	sqlite3 *m_pDb;
	sqlite3_stmt *m_pStmt;
	char m_aFilename[512];
	bool m_Setup;
	bool m_Done;
};

std::unique_ptr<IDbConnection> CreateSqliteConnection(const char *pFilename, bool Setup)
{
	return std::make_unique<CSqliteConnection>(pFilename, Setup);
}
