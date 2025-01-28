#ifndef ENGINE_SERVER_DATABASES_CONNECTION_H
#define ENGINE_SERVER_DATABASES_CONNECTION_H

#include <memory>

class IDbConnection
{
public:
    virtual ~IDbConnection() {}

    // Connection management
    virtual bool Connect(char* pError, int ErrorSize) = 0;
    virtual void Disconnect() = 0;

    // Statement preparation and execution
    virtual bool PrepareStatement(const char* pStmt, char* pError, int ErrorSize) = 0;
    virtual bool ExecuteUpdate(int* pNumUpdated, char* pError, int ErrorSize) = 0;
    virtual bool Step(bool* pEnd, char* pError, int ErrorSize) = 0;

    // Parameter binding
    virtual void BindString(int Idx, const char* pString) = 0;
    virtual void BindInt(int Idx, int Value) = 0;

    // Result retrieval
    virtual bool IsNull(int Col) = 0;
    virtual int GetInt(int Col) = 0;
    virtual void GetString(int Col, char* pBuffer, int BufferSize) = 0;

    // Rating system specific
    virtual bool AddPoints(const char* pPlayer, int Points, char* pError, int ErrorSize) = 0;
};

// Factory function declaration
std::unique_ptr<IDbConnection> CreateSqliteConnection(const char* pFilename, bool Setup);

#endif // ENGINE_SERVER_DATABASES_CONNECTION_H 