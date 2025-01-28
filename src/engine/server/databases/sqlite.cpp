#include "connection.h"

#include <sqlite3.h>
#include <base/system.h>
#include <engine/console.h>
#include <algorithm>

#include <atomic>

class CSqliteConnection : public IDbConnection
{
public:
	CSqliteConnection(const char *pFilename, bool Setup);
	~CSqliteConnection() override;
	void Print(IConsole *pConsole, const char *pMode) override;

	const char *BinaryCollate() const override { return "BINARY"; }
	void ToUnixTimestamp(const char *pTimestamp, char *aBuf, unsigned int BufferSize) override {}
	const char *InsertTimestampAsUtc() const override { return "DATETIME(?, 'utc')"; }
	const char *CollateNocase() const override { return "? COLLATE NOCASE"; }
	const char *InsertIgnore() const override { return "INSERT OR IGNORE"; }
	const char *Random() const override { return "RANDOM()"; }
	const char *MedianMapTime(char *pBuffer, int BufferSize) const override { return ""; }
	const char *False() const override { return "0"; }
	const char *True() const override { return "1"; }

	bool Connect(char *pError, int ErrorSize) override;
	void Disconnect() override;

	bool PrepareStatement(const char *pStmt, char *pError, int ErrorSize) override;

	void BindString(int Idx, const char *pString) override;
	void BindBlob(int Idx, unsigned char *pBlob, int Size) override;
	void BindInt(int Idx, int Value) override;
	void BindInt64(int Idx, int64_t Value) override;
	void BindFloat(int Idx, float Value) override;
	void BindNull(int Idx) override;

	void Print() override;
	bool Step(bool *pEnd, char *pError, int ErrorSize) override;
	bool ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize) override;

	bool IsNull(int Col) override;
	float GetFloat(int Col) override;
	int GetInt(int Col) override;
	int64_t GetInt64(int Col) override;
	void GetString(int Col, char *pBuffer, int BufferSize) override;
	int GetBlob(int Col, unsigned char *pBuffer, int BufferSize) override;

	bool AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize) override;

private:
	char m_aFilename[512];
	bool m_Setup;

	sqlite3 *m_pDb;
	sqlite3_stmt *m_pStmt;
	bool m_Done;
	bool Execute(const char *pQuery, char *pError, int ErrorSize);
	bool ConnectImpl(char *pError, int ErrorSize);
	bool FormatError(int Result, char *pError, int ErrorSize);
	void AssertNoError(int Result);

	std::atomic_bool m_InUse;
};

CSqliteConnection::CSqliteConnection(const char *pFilename, bool Setup) :
	IDbConnection("record"),
	m_Setup(Setup),
	m_pDb(nullptr),
	m_pStmt(nullptr),
	m_Done(true),
	m_InUse(false)
{
	str_copy(m_aFilename, pFilename, sizeof(m_aFilename));
}

CSqliteConnection::~CSqliteConnection()
{
	if(m_pStmt != nullptr)
	{
		sqlite3_finalize(m_pStmt);
		m_pStmt = nullptr;
	}
	
	if(m_pDb != nullptr)
	{
		sqlite3_close(m_pDb);
		m_pDb = nullptr;
	}

	if(sqlite3_temp_directory)
	{
		sqlite3_free(sqlite3_temp_directory);
		sqlite3_temp_directory = nullptr;
	}
}

void CSqliteConnection::Print(IConsole *pConsole, const char *pMode)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SQLite-%s: DB: '%s'",
		pMode, m_aFilename);
	pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

bool CSqliteConnection::Connect(char *pError, int ErrorSize)
{
	if(m_InUse.exchange(true))
	{
		str_copy(pError, "Database connection already in use", ErrorSize);
		return true;
	}
	if(ConnectImpl(pError, ErrorSize))
	{
		m_InUse.store(false);
		return true;
	}
	return false;
}

bool CSqliteConnection::ConnectImpl(char *pError, int ErrorSize)
{
	if(m_pDb != nullptr)
		return false;

	// Set temp directory for SQLite
	char aTempPath[512];
	fs_storage_path("tmp", aTempPath, sizeof(aTempPath));
	sqlite3_temp_directory = sqlite3_mprintf("%s", aTempPath);

	// Open database with additional flags for better memory handling
	int Flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
	int Result = sqlite3_open_v2(m_aFilename, &m_pDb, Flags, nullptr);
	
	if(Result != SQLITE_OK)
	{
		str_format(pError, ErrorSize, "Can't open sqlite database: '%s'", sqlite3_errmsg(m_pDb));
		if(m_pDb)
		{
			sqlite3_close(m_pDb);
			m_pDb = nullptr;
		}
		return true;
	}

	// Configure SQLite for better memory handling
	Execute("PRAGMA temp_store = MEMORY", pError, ErrorSize);
	Execute("PRAGMA journal_mode = WAL", pError, ErrorSize);
	Execute("PRAGMA synchronous = NORMAL", pError, ErrorSize);
	Execute("PRAGMA cache_size = 2000", pError, ErrorSize);
	Execute("PRAGMA mmap_size = 268435456", pError, ErrorSize);  // 256MB memory map

	sqlite3_busy_timeout(m_pDb, 1000);  // 1 second timeout

	if(m_Setup)
	{
		char aBuf[512];
		FormatCreateRatings(aBuf, sizeof(aBuf));
		if(Execute(aBuf, pError, ErrorSize))
			return true;
		m_Setup = false;
	}
	return false;
}

void CSqliteConnection::Disconnect()
{
	if(m_pStmt != nullptr)
		sqlite3_finalize(m_pStmt);
	m_pStmt = nullptr;
	m_InUse.store(false);
}

bool CSqliteConnection::PrepareStatement(const char *pStmt, char *pError, int ErrorSize)
{
	if(m_pStmt != nullptr)
		sqlite3_finalize(m_pStmt);
	m_pStmt = nullptr;
	int Result = sqlite3_prepare_v2(m_pDb, pStmt, -1, &m_pStmt, NULL);
	if(FormatError(Result, pError, ErrorSize))
		return true;
	m_Done = false;
	return false;
}

void CSqliteConnection::BindString(int Idx, const char *pString)
{
	int Result = sqlite3_bind_text(m_pStmt, Idx, pString, -1, NULL);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindBlob(int Idx, unsigned char *pBlob, int Size)
{
	int Result = sqlite3_bind_blob(m_pStmt, Idx, pBlob, Size, NULL);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindInt(int Idx, int Value)
{
	int Result = sqlite3_bind_int(m_pStmt, Idx, Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindInt64(int Idx, int64_t Value)
{
	int Result = sqlite3_bind_int64(m_pStmt, Idx, Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindFloat(int Idx, float Value)
{
	int Result = sqlite3_bind_double(m_pStmt, Idx, (double)Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindNull(int Idx)
{
	int Result = sqlite3_bind_null(m_pStmt, Idx);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::Print() {}

bool CSqliteConnection::Step(bool *pEnd, char *pError, int ErrorSize)
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
	else
	{
		if(FormatError(Result, pError, ErrorSize))
			return true;
	}
	*pEnd = true;
	return false;
}

bool CSqliteConnection::ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize)
{
	bool End;
	if(Step(&End, pError, ErrorSize))
		return true;
	*pNumUpdated = sqlite3_changes(m_pDb);
	return false;
}

bool CSqliteConnection::IsNull(int Col)
{
	return sqlite3_column_type(m_pStmt, Col - 1) == SQLITE_NULL;
}

float CSqliteConnection::GetFloat(int Col)
{
	return (float)sqlite3_column_double(m_pStmt, Col - 1);
}

int CSqliteConnection::GetInt(int Col)
{
	return sqlite3_column_int(m_pStmt, Col - 1);
}

int64_t CSqliteConnection::GetInt64(int Col)
{
	return sqlite3_column_int64(m_pStmt, Col - 1);
}

void CSqliteConnection::GetString(int Col, char *pBuffer, int BufferSize)
{
	str_copy(pBuffer, (const char *)sqlite3_column_text(m_pStmt, Col - 1), BufferSize);
}

int CSqliteConnection::GetBlob(int Col, unsigned char *pBuffer, int BufferSize)
{
	int Size = sqlite3_column_bytes(m_pStmt, Col - 1);
	Size = std::min(Size, BufferSize);
	mem_copy(pBuffer, sqlite3_column_blob(m_pStmt, Col - 1), Size);
	return Size;
}

bool CSqliteConnection::Execute(const char *pQuery, char *pError, int ErrorSize)
{
	char *pErrorMsg;
	int Result = sqlite3_exec(m_pDb, pQuery, NULL, NULL, &pErrorMsg);
	if(Result != SQLITE_OK)
	{
		str_format(pError, ErrorSize, "error executing query: '%s'", pErrorMsg);
		sqlite3_free(pErrorMsg);
		return true;
	}
	return false;
}

bool CSqliteConnection::FormatError(int Result, char *pError, int ErrorSize)
{
	if(Result != SQLITE_OK)
	{
		str_copy(pError, sqlite3_errmsg(m_pDb), ErrorSize);
		return true;
	}
	return false;
}

void CSqliteConnection::AssertNoError(int Result)
{
	char aBuf[128];
	if(FormatError(Result, aBuf, sizeof(aBuf)))
	{
		dbg_msg("sqlite", "unexpected sqlite error: %s", aBuf);
		dbg_assert(0, "sqlite error");
	}
}

bool CSqliteConnection::AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"INSERT INTO %s_points(Name, Points) "
		"VALUES (?, ?) "
		"ON CONFLICT(Name) DO UPDATE SET Points=Points+?",
		GetPrefix());
	if(PrepareStatement(aBuf, pError, ErrorSize))
		return true;
	BindString(1, pPlayer);
	BindInt(2, Points);
	BindInt(3, Points);
	bool End;
	return Step(&End, pError, ErrorSize);
}

std::unique_ptr<IDbConnection> CreateSqliteConnection(const char *pFilename, bool Setup)
{
	return std::make_unique<CSqliteConnection>(pFilename, Setup);
}
