/* (c) KeksTW. */
#ifndef GAME_SERVER_GAMEMODES_FNG2_H
#define GAME_SERVER_GAMEMODES_FNG2_H

#include <game/server/gamecontroller.h>
#include <base/vmath.h>
#include <engine/server/databases/connection.h>

class CGameControllerFNG2 : public IGameController
{
public:
	CGameControllerFNG2(class CGameContext* pGameServer);
	CGameControllerFNG2(class CGameContext* pGameServer, CConfiguration& pConfig);
	virtual void Tick();
	virtual void Snap(int SnappingClient);
	virtual void OnCharacterSpawn(class CCharacter *pChr);
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);
	
	virtual void DoWincheck();
	
	virtual void PostReset();
protected:
	void EndRound();	
	void UpdatePlayerRating(const char* pName, int Points);
private:
	std::unique_ptr<IDbConnection> m_pDatabase;
};
#endif
