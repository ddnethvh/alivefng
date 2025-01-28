#include "connection.h"
#include <base/system.h>

IDbConnection::IDbConnection(const char *pPrefix)
{
	str_copy(m_aPrefix, pPrefix, sizeof(m_aPrefix));
}

void IDbConnection::FormatCreateRatings(char *aBuf, unsigned int BufferSize) const
{
	str_format(aBuf, BufferSize,
		"CREATE TABLE IF NOT EXISTS %s_ratings ("
		"  Name VARCHAR(%d) COLLATE %s NOT NULL, "
		"  Rating INT DEFAULT 1000, "
		"  PRIMARY KEY (Name)"
		")",
		GetPrefix(), MAX_NAME_LENGTH_SQL, BinaryCollate());
}
