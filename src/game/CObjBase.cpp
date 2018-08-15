
#include "../common/CException.h"
#include "../common/CResourceLock.h"
#include "../common/CUIDExtra.h"
#include "../common/sphereversion.h"
#include "../common/CLog.h"
#include "../network/network.h"
#include "../network/send.h"
#include "../sphere/ProfileTask.h"
#include "chars/CChar.h"
#include "clients/CClient.h"
#include "components/CCSpawn.h"
#include "components/CCItemDamageable.h"
#include "CObjBase.h"
#include "spheresvr.h"
#include "triggers.h"

bool CObjBaseTemplate::IsDeleted() const
{
	ADDTOCALLSTACK("CObjBaseTemplate::IsDeleted");
	return (!m_UID.IsValidUID() || ( GetParent() == &g_World.m_ObjDelete ));
}

int CObjBaseTemplate::IsWeird() const
{
	ADDTOCALLSTACK_INTENSIVE("CObjBaseTemplate::IsWeird");
	if ( !GetParent() )
		return 0x3101;

	if ( !IsValidUID() )
		return 0x3102;

	return 0;
}

bool GetDeltaStr( CPointMap & pt, tchar * pszDir )
{
	tchar * ppCmd[3];
	size_t iQty = Str_ParseCmds( pszDir, ppCmd, CountOf(ppCmd));
	if (iQty <= 0)
		return false;

	tchar chDir = static_cast<tchar>(toupper(ppCmd[0][0]));
	int iTmp = Exp_GetVal( ppCmd[1] );

	if ( IsDigit( chDir ) || chDir == '-' )
	{
		pt.m_x += (Exp_GetSVal(ppCmd[0]));
		pt.m_y += (short)(iTmp);
		pt.m_z += (Exp_GetCVal(ppCmd[2]));
	}
	else	// a direction by name.
	{
		if ( iTmp == 0 )
			iTmp = 1;
		DIR_TYPE eDir = GetDirStr( ppCmd[0] );
		if ( eDir >= DIR_QTY )
			return false;
		pt.MoveN( eDir, iTmp );
	}

	return true;
}

/////////////////////////////////////////////////////////////////
// -CObjBase stuff
// Either a player, npc or item.

CObjBase::CObjBase( bool fItem )
{
	sm_iCount ++;
	m_iCreatedResScriptIdx = (size_t)-1;
	m_iCreatedResScriptLine = -1;
	m_RunningTrigger = NULL;
	m_CallingObjTrigger = NULL;

	m_wHue = HUE_DEFAULT;
	m_timeout.Init();
	m_timestamp.Init();

	m_CanMask = 0;
	m_ModAr = 0;
	m_ModMaxWeight = 0;
    _uidSpawn.InitUID();

	m_fStatusUpdate = 0;
	m_PropertyList = NULL;
	m_PropertyHash = 0;
	m_PropertyRevision = 0;

	if ( g_Serv.IsLoading())
	{
		// Don't do this yet if we are loading. UID will be set later.
		// Just say if this is an item or not.
		CObjBaseTemplate::SetUID(UID_O_DISCONNECT|UID_O_INDEX_MASK|(fItem ? UID_F_ITEM : 0));
	}
	else
	{
		// Find a free UID slot for this.
		SetUID(UID_CLEAR, fItem);
		ASSERT(IsValidUID());
		SetContainerFlags(UID_O_DISCONNECT);	// it is no place for now
	}

	// Put in the idle list by default. (til placed in the world)
	g_World.m_ObjNew.InsertHead( this );
}

CObjBase::~CObjBase()
{
	FreePropertyList();
	g_World.m_ObjStatusUpdates.RemovePtr(this);

	--sm_iCount;
	ASSERT( IsDisconnected());

	// free up the UID slot.
	SetUID( UID_UNUSED, false );
}

bool CObjBase::IsContainer() const
{
	ADDTOCALLSTACK("CObjBase::IsContainer");
	// Simple test if object is a container.
	return( dynamic_cast <const CContainer*>(this) != NULL );
}

void CObjBase::SetHue( HUE_TYPE wHue, bool bAvoidTrigger, CTextConsole *pSrc, CObjBase *SourceObj, llong sound )
{
	if (g_Serv.IsLoading()) //We do not want tons of @Dye being called during world load, just set the hue then continue...
	{
		m_wHue = wHue;
		return;
	}

	CScriptTriggerArgs args;
	args.m_iN1=wHue;
	args.m_iN2=sound;

	/*	@Dye is now more universal, it is called on EVERY CObjBase color change.
		Sanity checks are recommended and if possible, avoid using it on universal events. */

	/*	Trigger info to be added to intenal
		lpctstr const CItem::sm_szTrigName	//CItem.cpp
		lpctstr const CChar::sm_szTrigName	//CChar.cpp
		enum ITRIG_TYPE						//CObjBase.h
		enum CTRIG_TYPE						//CObjBase.h
		ADD(DYE,					"@Dye")	//triggers.tbl
	*/

	if (!bAvoidTrigger)
	{
		if (IsTrigUsed("@Dye"))
		{
			TRIGRET_TYPE iRet;

			if (SourceObj)
				args.m_pO1 = SourceObj;

			//lpctstr sTrig = (IsChar() ? CChar::sm_szTrigName[CTRIG_DYE] : CItem::sm_szTrigName[ITRIG_DYE]);

			iRet = OnTrigger("@Dye", pSrc, &args);

			if (iRet == TRIGRET_RET_TRUE)
				return;
		}
	}

	if (args.m_iN2 > 0) //No sound? No checks for who can hear, packets....
		Sound((SOUND_TYPE)(args.m_iN2));

	m_wHue = (SOUND_TYPE)(args.m_iN1);
}

HUE_TYPE CObjBase::GetHue() const
{
	return( m_wHue );
}

word CObjBase::GetHueAlt() const
{
	// IT_EQ_MEMORY_OBJ = MEMORY_TYPE mask
	// IT_EQ_VENDOR_BOX = restock time.
	return( m_wHue );
}
void CObjBase::SetHueAlt( HUE_TYPE wHue )
{
	m_wHue = wHue;
}

int CObjBase::IsWeird() const
{
	ADDTOCALLSTACK_INTENSIVE("CObjBase::IsWeird");
	int iResultCode = CObjBaseTemplate::IsWeird();
	if ( iResultCode )
	{
		return( iResultCode );
	}
	if ( ! g_Serv.IsLoading())
	{
		if ( GetUID().ObjFind() != this )	// make sure it's linked both ways correctly.
		{
			return( 0x3201 );
		}
	}
	return 0;
}

void CObjBase::SetUID( dword dwIndex, bool fItem )
{
	ADDTOCALLSTACK("CObjBase::SetUID");
	// Move the serial number,
	// This is possibly dangerous if conflict arrises.

	dwIndex &= UID_O_INDEX_MASK;	// Make sure no flags in here.
	if ( IsValidUID())	// i already have a uid ?
	{
		if ( ! dwIndex )
			return;	// The point was just to make sure it was located.
		// remove the old UID.
		g_World.FreeUID( ((dword)GetUID()) & UID_O_INDEX_MASK );
	}

	if ( dwIndex != UID_O_INDEX_MASK )	// just wanted to remove it
		dwIndex = g_World.AllocUID( dwIndex, this );

	if ( fItem )
		dwIndex |= UID_F_ITEM;

	CObjBaseTemplate::SetUID( dwIndex );
}


lpctstr CObjBase::GetName() const	// resolve ambiguity w/CScriptObj
{
	return( CObjBaseTemplate::GetName());
}

lpctstr CObjBase::GetResourceName() const
{
	return Base_GetDef()->GetResourceName();
}

void inline CObjBase::SetNamePool_Fail( tchar * ppTitles )
{
	ADDTOCALLSTACK("CObjBase::SetNamePool_Fail");
	DEBUG_ERR(( "Name pool '%s' could not be found\n", ppTitles ));
	CObjBase::SetName( ppTitles );
}

bool CObjBase::SetNamePool( lpctstr pszName )
{
	ADDTOCALLSTACK("CObjBase::SetNamePool");
	ASSERT(pszName);

	// Parse out the name from the name pool ?
	if ( pszName[0] == '#' )
	{
		++pszName;
		tchar *pszTmp = Str_GetTemp();
		strcpy( pszTmp, pszName );

		tchar * ppTitles[2];
		Str_ParseCmds( pszTmp, ppTitles, CountOf(ppTitles));

		CResourceLock s;
		if ( ! g_Cfg.ResourceLock( s, RES_NAMES, ppTitles[0] ))
		{
			SetNamePool_Fail( ppTitles[0] );
			return false;
		}

		// Pick a random name.
		if ( ! s.ReadKey())
		{
			SetNamePool_Fail( ppTitles[0] );
			return false;
		}
		int iCount = Calc_GetRandVal2( 1, ATOI( s.GetKey()) );
		while ( iCount > 0 )
		{
			if ( ! s.ReadKey())
			{
				SetNamePool_Fail( ppTitles[0] );
				return false;
			}
			--iCount;
		}

		if ( CObjBaseTemplate::SetName( s.GetKey() ) == false )
			return false;
	}
	else
	{
		lpctstr pszTmp = pszName;

		// NOTE: Name must be <= MAX_NAME_SIZE
		tchar szTmp[ MAX_ITEM_NAME_SIZE + 1 ];
		if ( strlen( pszName ) >= MAX_ITEM_NAME_SIZE )
		{
			strncpynull( szTmp, pszName, MAX_ITEM_NAME_SIZE );
			pszTmp = szTmp;
		}

		// Can't be a dupe name with type ?
		lpctstr pszTypeName = Base_GetDef()->GetTypeName();
		if ( ! strcmpi( pszTypeName, pszTmp ))
			pszTmp = "";

		if ( CObjBaseTemplate::SetName( pszTmp ) == false )
			return false;
	}

	UpdatePropertyFlag(AUTOTOOLTIP_FLAG_NAME);
	return true;
}

bool CObjBase::MoveNearObj( const CObjBaseTemplate *pObj, word iSteps )
{
	ADDTOCALLSTACK("CObjBase::MoveNearObj");
	ASSERT(pObj);
	if ( pObj->IsDisconnected() )	// nothing is "near" a disconnected item.
		return false;

	pObj = pObj->GetTopLevelObj();
	return MoveNear(pObj->GetTopPoint(), iSteps);
}

void CObjBase::r_WriteSafe( CScript & s )
{
	ADDTOCALLSTACK("CObjBase::r_WriteSafe");
	// Write an object with some fault protection.
	dword uid = 0;
	try
	{
		uid = GetUID();

		//	objects with TAG.NOSAVE set are not saved
		if ( m_TagDefs.GetKeyNum("NOSAVE", true) )
			return;

		if ( !g_Cfg.m_fSaveGarbageCollect )
		{
			if ( g_World.FixObj(this) )
				return;
		}
		r_Write(s);
	}
	catch ( const CSError& e )
	{
		g_Log.CatchEvent(&e, "Write Object 0%x", uid);
		CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
	}
	catch (...)	// catch all
	{
		g_Log.CatchEvent(NULL, "Write Object 0%x", uid);
		CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
	}
}

void CObjBase::SetTimeout( int64 iDelayInTicks )
{
	ADDTOCALLSTACK("CObjBase::SetTimeout");
	// Set delay in TICK_PER_SEC of a sec. -1 = never.
	if ( iDelayInTicks < 0 )
		m_timeout.Init();
	else
		m_timeout = CServerTime::GetCurrentTime() + iDelayInTicks;
}

int64 CObjBase::GetTimerDiff() const
{
    // How long till this will expire ?
    return g_World.GetTimeDiff( m_timeout );
    // return: < 0 = in the past ( m_timeout - CServerTime::GetCurrentTime() )
}

int64 CObjBase::GetTimerAdjusted() const
{
	// RETURN: time in seconds from now.
	if ( ! IsTimerSet())
		return -1;
	int64 iDiffInTicks = GetTimerDiff();
	if ( iDiffInTicks < 0 )
		return 0;
	return ( iDiffInTicks / TICK_PER_SEC );
}

int64 CObjBase::GetTimerDAdjusted() const
{
	// RETURN: time in seconds from now.
	if ( ! IsTimerSet())
		return -1;
	int64 iDiffInTicks = GetTimerDiff();
	if ( iDiffInTicks < 0 )
		return 0;
	return iDiffInTicks;
}

void CObjBase::Sound( SOUND_TYPE id, int iOnce ) const // Play sound effect for player
{
	ADDTOCALLSTACK("CObjBase::Sound");
	// play for everyone near by.

	if (( id <= 0 ) || !g_Cfg.m_fGenericSounds )
		return;

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if ( ! pClient->CanHear( this, TALKMODE_OBJ ) )
			continue;
		pClient->addSound( id, this, iOnce );
	}
}

void CObjBase::Effect(EFFECT_TYPE motion, ITEMID_TYPE id, const CObjBase * pSource, byte bSpeedSeconds, byte bLoop, bool fExplode, dword color, dword render, word effectid, word explodeid, word explodesound, dword effectuid, byte type) const
{
	ADDTOCALLSTACK("CObjBase::Effect");
	// show for everyone near by.
	//
	// bSpeedSeconds
	// bLoop
	// fExplode

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if ( ! pClient->CanSee( this ) )
			continue;

		// Given the same bLoop, Enhanced Client shows the effect for a much shorter amount of time than Classic Client,
		//	so it may be a nice idea to adjust it automatically.
		byte bLoopAdjusted = bLoop;
		if (pClient->GetNetState()->isClientEnhanced())
			bLoopAdjusted *= 3;

		pClient->addEffect(motion, id, this, pSource, bSpeedSeconds, bLoopAdjusted, fExplode, color, render, effectid, explodeid, explodesound, effectuid, type);
	}
}

void CObjBase::Effect(EFFECT_TYPE motion, ITEMID_TYPE id, CPointMap & pt ,const CObjBase * pSource, byte bSpeedSeconds, byte bLoop, bool fExplode, dword color, dword render, word effectid, word explodeid, word explodesound, dword effectuid, byte type) const
{
	ADDTOCALLSTACK("CObjBase::Effect");
	// show for everyone near by.
	//
	// bSpeedSeconds
	// bLoop
	// fExplode

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if (!pClient->CanSee(this))
			continue;

		// Given the same bLoop, Enhanced Client shows the effect for a much shorter amount of time than Classic Client,
		//	so it may be a nice idea to adjust it automatically.
		byte bLoopAdjusted = bLoop;
		if (pClient->GetNetState()->isClientEnhanced())
			bLoopAdjusted *= 3;

		pClient->addEffect(motion, id, pt, pSource, bSpeedSeconds, bLoopAdjusted, fExplode, color, render, effectid, explodeid, explodesound, effectuid, type);
	}
}

void CObjBase::Emote(lpctstr pText, CClient * pClientExclude, bool fForcePossessive)
{
	ADDTOCALLSTACK("CObjBase::Emote");
	// IF this is not the top level object then it might be possessive ?

	// "*You see NAME blah*" or "*You blah*"
	// fPosessive = "*You see NAME's blah*" or "*Your blah*"

	CObjBase *pObjTop = static_cast<CObjBase*>(GetTopLevelObj());
	if ( !pObjTop )
		return;

	tchar *pszThem = Str_GetTemp();
	tchar *pszYou = Str_GetTemp();

	if ( pObjTop->IsChar() )
	{
		// Someone has this equipped.

		if ( pObjTop != this )
		{
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_1), pObjTop->GetName(), GetName(), pText);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_2), GetName(), pText);
		}
		else if ( fForcePossessive )
		{
			// ex. "You see joes poor shot ruin an arrow"
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_3), GetName(), pText);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_4), pText);
		}
		else
		{
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_5), GetName(), pText);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_6), pText);
		}
	}
	else
	{
		// Top level is an item. Article ?
		sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_7), GetName(), pText);
		strcpy(pszYou, pszThem);
	}

	pObjTop->UpdateObjMessage(pszThem, pszYou, pClientExclude, HUE_TEXT_DEF, TALKMODE_EMOTE);
}

void CObjBase::Emote2(lpctstr pText, lpctstr pText1, CClient * pClientExclude, bool fForcePossessive)
{
	ADDTOCALLSTACK("CObjBase::Emote");
	// IF this is not the top level object then it might be possessive ?

	// "*You see NAME blah*" or "*You blah*"
	// fPosessive = "*You see NAME's blah*" or "*Your blah*"

	CObjBase *pObjTop = static_cast<CObjBase*>(GetTopLevelObj());
	if ( !pObjTop )
		return;

	tchar *pszThem = Str_GetTemp();
	tchar *pszYou = Str_GetTemp();

	if ( pObjTop->IsChar() )
	{
		// Someone has this equipped.
		if ( pObjTop != this )
		{
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_1), pObjTop->GetName(), GetName(), pText1);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_2), GetName(), pText);
		}
		else if ( fForcePossessive )
		{
			// ex. "You see joes poor shot ruin an arrow"
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_3), GetName(), pText1);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_4), pText);
		}
		else
		{
			sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_5), GetName(), pText1);
			sprintf(pszYou, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_6), pText);
		}
	}
	else
	{
		// Top level is an item. Article ?
		sprintf(pszThem, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EMOTE_7), GetName(), pText1);
		strcpy(pszYou, pszThem);
	}

	pObjTop->UpdateObjMessage(pszThem, pszYou, pClientExclude, HUE_TEXT_DEF, TALKMODE_EMOTE);
}

// Speak to all clients in the area.
// ASCII packet
void CObjBase::Speak( lpctstr pText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font )
{
	ADDTOCALLSTACK_INTENSIVE("CObjBase::Speak");
	g_World.Speak( this, pText, wHue, mode, font );
}

// Speak to all clients in the area.
// Unicode packet
void CObjBase::SpeakUTF8( lpctstr pText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font, CLanguageID lang )
{
	ADDTOCALLSTACK_INTENSIVE("CObjBase::SpeakUTF8");
	// convert UTF8 to UNICODE.
	nchar szBuffer[ MAX_TALK_BUFFER ];
	CvtSystemToNUNICODE( szBuffer, CountOf(szBuffer), pText, -1 );
	g_World.SpeakUNICODE( this, szBuffer, wHue, mode, font, lang );
}

// Speak to all clients in the area.
// Unicode packet
// Difference with SpeakUTF8: this method accepts as text input an nword, which is unicode if sphere is compiled with UNICODE macro)
void CObjBase::SpeakUTF8Ex( const nword * pText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font, CLanguageID lang )
{
	ADDTOCALLSTACK_INTENSIVE("CObjBase::SpeakUTF8Ex");
	g_World.SpeakUNICODE( this, pText, wHue, mode, font, lang );
}

bool CObjBase::MoveNear( CPointMap pt, word iSteps )
{
	ADDTOCALLSTACK("CObjBase::MoveNear");
	// Move to nearby this other object.
	// Actually move it within +/- iSteps

	CPointBase ptOld = pt;
	for ( uint i = 0; i < iSteps; ++i )
	{
		pt = ptOld;
		pt.m_x += (short)Calc_GetRandVal2(-iSteps, iSteps);
		pt.m_y += (short)Calc_GetRandVal2(-iSteps, iSteps);

		if ( !pt.IsValidPoint() )	// hit the edge of the world, so go back to the previous valid position
		{
			pt = ptOld;
			break;
		}
	}

	if ( IsChar() )
	{
		// Don't move to an position that we can't walk to
		CChar *pChar = static_cast<CChar *>(this);
		ASSERT(pChar);

		pChar->m_zClimbHeight = 0;
		if ( pChar->CanMoveWalkTo(pt, false) == NULL )
			return false;
	}

	if ( MoveTo(pt) )
	{
		if ( IsItem() )
			Update();
		return true;
	}
	return false;
}

void CObjBase::UpdateObjMessage( lpctstr pTextThem, lpctstr pTextYou, CClient * pClientExclude, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font, bool bUnicode ) const
{
	ADDTOCALLSTACK("CObjBase::UpdateObjMessage");
	// Show everyone a msg coming from this object.

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if ( pClient == pClientExclude )
			continue;
		if ( ! pClient->CanSee( this ))
			continue;

		pClient->addBarkParse(( pClient->GetChar() == this )? pTextYou : pTextThem, this, wHue, mode, font, bUnicode );
	}
}

void CObjBase::UpdateCanSee(PacketSend *packet, CClient *exclude) const
{
	ADDTOCALLSTACK("CObjBase::UpdateCanSee");
	// Send this update message to everyone who can see this.
	// NOTE: Need not be a top level object. CanSee() will calc that.

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if (( pClient == exclude ) || !pClient->CanSee(this) )
			continue;

		packet->send(pClient);
	}
	delete packet;
}

TRIGRET_TYPE CObjBase::OnHearTrigger( CResourceLock & s, lpctstr pszCmd, CChar * pSrc, TALKMODE_TYPE & mode, HUE_TYPE wHue)
{
	ADDTOCALLSTACK("CObjBase::OnHearTrigger");
	// Check all the keys in this script section.
	// look for pattern or partial trigger matches.
	// RETURN:
	//  TRIGRET_ENDIF = no match.
	//  TRIGRET_DEFAULT = found match but it had no RETURN
	CScriptTriggerArgs Args( pszCmd );
	Args.m_iN1 = mode;
	Args.m_iN2 = wHue;

	bool fMatch = false;

	while ( s.ReadKeyParse())
	{
		if ( s.IsKeyHead("ON",2))
		{
			// Look for some key word.
			_strupr( s.GetArgStr());
			if ( Str_Match( s.GetArgStr(), pszCmd ) == MATCH_VALID )
				fMatch = true;
			continue;
		}

		if ( ! fMatch )
			continue;	// look for the next "ON" section.

		TRIGRET_TYPE iRet = CObjBase::OnTriggerRunVal( s, TRIGRUN_SECTION_EXEC, pSrc, &Args );
		if ( iRet != TRIGRET_RET_FALSE )
			return( iRet );

		fMatch = false;
	}

	mode = static_cast<TALKMODE_TYPE>(Args.m_iN1);
	return( TRIGRET_ENDIF );	// continue looking.
}

enum OBR_TYPE
{
	OBR_ROOM,
	OBR_SECTOR,
	OBR_SPAWNITEM,
	OBR_TOPOBJ,
	OBR_TYPEDEF,
	OBR_QTY
};

lpctstr const CObjBase::sm_szRefKeys[OBR_QTY+1] =
{
	"ROOM",
	"SECTOR",
	"SPAWNITEM",
	"TOPOBJ",
	"TYPEDEF",
	NULL
};

bool CObjBase::r_GetRef( lpctstr & pszKey, CScriptObj * & pRef )
{
	ADDTOCALLSTACK("CObjBase::r_GetRef");
	int i = FindTableHeadSorted( pszKey, sm_szRefKeys, CountOf(sm_szRefKeys)-1 );
	if ( i >= 0 )
	{
		pszKey += strlen( sm_szRefKeys[i] );
		SKIP_SEPARATORS(pszKey);
		switch (i)
		{
			case OBR_ROOM:
				pRef = GetTopLevelObj()->GetTopPoint().GetRegion( REGION_TYPE_ROOM );
				return true;
			case OBR_SECTOR:
				pRef = GetTopLevelObj()->GetTopSector();
				return true;
			case OBR_SPAWNITEM:
            {
                if (_uidSpawn != UID_UNUSED && pszKey[-1] != '.')
                    break;
                CItem *pItem = static_cast<CItem*>(_uidSpawn.ItemFind());
                if (pItem)
                {
                    pRef = pItem;
                }
            }
				return true;
			case OBR_TOPOBJ:
				if ( pszKey[-1] != '.' )	// only used as a ref !
					break;
				pRef = dynamic_cast <CObjBase*>(GetTopLevelObj());
				return true;
			case OBR_TYPEDEF:
				pRef = Base_GetDef();
				return true;
		}

	}
	return CScriptObj::r_GetRef(pszKey, pRef);
}

enum OBC_TYPE
{
	#define ADD(a,b) OC_##a,
	#include "../tables/CObjBase_props.tbl"
	#undef ADD
	OC_QTY
};

lpctstr const CObjBase::sm_szLoadKeys[OC_QTY+1] =
{
	#define ADD(a,b) b,
	#include "../tables/CObjBase_props.tbl"
	#undef ADD
	NULL
};

bool CObjBase::r_WriteVal( lpctstr pszKey, CSString &sVal, CTextConsole * pSrc )
{
	ADDTOCALLSTACK("CObjBase::r_WriteVal");
	EXC_TRY("WriteVal");

    int index = FindTableHeadSorted( pszKey, sm_szLoadKeys, CountOf( sm_szLoadKeys )-1 );
    if ( index < 0 )
        {
        // RES_FUNCTION call
        // Is it a function returning a value ? Parse args ?
            lpctstr pszArgs = strchr(pszKey, ' ');
        if ( pszArgs != NULL )
        {
            pszArgs++;
            SKIP_SEPARATORS(pszArgs);
        }

        CScriptTriggerArgs Args( pszArgs != NULL ? pszArgs : "" );
        if (r_Call(pszKey, pSrc, &Args, &sVal))
        {
            return true;
        }

        // Just try to default to something reasonable ?
        // Even though we have not really specified it correctly !

        // WORLD. ?
        if (g_World.r_WriteVal(pszKey, sVal, pSrc))
        {
            return true;
        }


        // TYPEDEF. ?
        if (Base_GetDef()->r_WriteVal(pszKey, sVal, pSrc))
        {
            return true;
        }
		return CScriptObj::r_WriteVal( pszKey, sVal, pSrc );
	}

	bool	fZero	= false;
	switch (index)
	{
		//return as string or hex number or NULL if not set
		//On these ones, check BaseDef if not found on dynamic
		case OC_NAMELOC:
		case OC_HITSPELL:
		case OC_SLAYER:
		case OC_SLAYERLESSER:
		case OC_SLAYERMISC:
		case OC_SLAYERSUPER:
		case OC_ABILITYPRIMARY:
		case OC_ABILITYSECONDARY:
		case OC_MANABURST:
			{
				CVarDefCont * pVar = GetDefKey(pszKey, true);
				sVal = pVar ? pVar->GetValStr() : "";
			}
			break;
		//return as decimal number or 0 if not set
		//On these ones, check BaseDef if not found on dynamic
		case OC_CASTINGFOCUS:
		case OC_DAMCHAOS:
		case OC_DAMCOLD:
		case OC_DAMDIRECT:
		case OC_DAMENERGY:
		case OC_DAMFIRE:
		case OC_DAMMODIFIER:
		case OC_DAMPHYSICAL:
		case OC_DAMPOISON:
		case OC_DECREASEHITCHANCE:
		case OC_EATERCOLD:
		case OC_EATERDAM:
		case OC_EATERENERGY:
		case OC_EATERFIRE:
		case OC_EATERKINETIC:
		case OC_EATERPOISON:
		case OC_ENHANCEPOTIONS:
		case OC_EXPANSION:
		case OC_FASTERCASTING:
		case OC_FASTERCASTRECOVERY:
		case OC_HITAREACOLD:
		case OC_HITAREAENERGY:
		case OC_HITAREAFIRE:
		case OC_HITAREAPHYSICAL:
		case OC_HITAREAPOISON:
		case OC_HITCURSE:
		case OC_HITDISPEL:
		case OC_HITFATIGUE:
		case OC_HITFIREBALL:
		case OC_HITHARM:
		case OC_HITLEECHLIFE:
		case OC_HITLEECHMANA:
		case OC_HITLEECHSTAM:
		case OC_HITLIGHTNING:
		case OC_HITLOWERATK:
		case OC_HITLOWERDEF:
		case OC_HITMAGICARROW:
		case OC_HITMANADRAIN:
		case OC_HITSPELLSTR:
		case OC_INCREASEDAM:
		case OC_INCREASEDEFCHANCE:
		case OC_INCREASEDEFCHANCEMAX:
		case OC_INCREASEGOLD:
		case OC_INCREASEHITCHANCE:
		case OC_INCREASEKARMALOSS:
		case OC_INCREASESPELLDAM:
		case OC_INCREASESWINGSPEED:
		case OC_LOWERREAGENTCOST:
		case OC_LOWERMANACOST:
		case OC_LOWERAMMOCOST:
		case OC_LOWERREQ:
		case OC_LUCK:
		case OC_MANABURSTFREQUENCY:
		case OC_MANABURSTKARMA:
		case OC_NIGHTSIGHT:
		case OC_RAGEFOCUS:
		case OC_REACTIVEPARALYZE:
		case OC_REFLECTPHYSICALDAM:
		case OC_REGENFOOD:
		case OC_REGENHITS:
		case OC_REGENMANA:
		case OC_REGENSTAM:
		case OC_REGENVALFOOD:
		case OC_REGENVALHITS:
		case OC_REGENVALMANA:
		case OC_REGENVALSTAM:
		case OC_RESCOLD:
		case OC_RESFIRE:
		case OC_COMBATBONUSSTAT:
		case OC_COMBATBONUSPERCENT:
		case OC_RESENERGY:
		case OC_RESPOISON:
		case OC_RESCOLDMAX:
		case OC_RESFIREMAX:
		case OC_RESENERGYMAX:
		case OC_RESPHYSICALMAX:
		case OC_RESPOISONMAX:
		case OC_RESONANCECOLD:
		case OC_RESONANCEENERGY:
		case OC_RESONANCEFIRE:
		case OC_RESONANCEKINETIC:
		case OC_RESONANCEPOISON:
		case OC_SOULCHARGE:
		case OC_SOULCHARGECOLD:
		case OC_SOULCHARGEENERGY:
		case OC_SOULCHARGEFIRE:
		case OC_SOULCHARGEKINETIC:
		case OC_SOULCHARGEPOISON:
		case OC_SPELLCHANNELING:
		case OC_SPELLCONSUMPTION:
		case OC_SPELLFOCUSING:
		case OC_VELOCITY:
		case OC_WEIGHTREDUCTION:
		case OC_RESPHYSICAL:
			{
				CVarDefCont * pVar = GetDefKey(pszKey, true);
				sVal.FormatLLVal(pVar ? pVar->GetValNum() : 0);
			}
			break;
        case OC_RECIPEALCHEMY:
        case OC_RECIPEBLACKSMITH:
        case OC_RECIPEBOWCRAFT:
        case OC_RECIPECARPENTRY:
        case OC_RECIPECARTOPGRAHY:
        case OC_RECIPECOOKING:
        case OC_RECIPEGLASSBLOWING:
        case OC_RECIPEINSCRIPTION:
        case OC_RECIPEMASONRY:
        case OC_RECIPETAILORING:
        case OC_RECIPETINKERING:
        {
            CVarDefCont * pVar = GetDefKey(pszKey, true);
            sVal.FormatLLHex(pVar ? pVar->GetValNum() : 0);
        }
        break;

		case OC_ARMOR:
			{
				if ( IsChar() )
				{
					CChar * pChar = static_cast<CChar*>(this);
					sVal.FormatVal( pChar->m_defense );
					break;
				} else {
					pszKey += strlen(sm_szLoadKeys[index]); // 9;
					if ( *pszKey == '.' )
					{
						SKIP_SEPARATORS( pszKey );

						if ( !strnicmp( pszKey, "LO", 2 ) )
						{
							sVal.Format( "%d", m_defenseBase );
						}
						else if ( !strnicmp( pszKey, "HI", 2 ) )
						{
							sVal.Format( "%d", m_defenseBase+m_defenseRange );
						}
					}
					else
					{
						sVal.Format( "%d,%d", m_defenseBase, m_defenseBase+m_defenseRange );
					}
				} break;
			}
		case OC_DAM:
			{
				pszKey += strlen(sm_szLoadKeys[index]); // 9;
				if ( *pszKey == '.' )
				{
					SKIP_SEPARATORS( pszKey );

					if ( !strnicmp( pszKey, "LO", 2 ) )
						sVal.Format( "%d", m_attackBase );
					else if ( !strnicmp( pszKey, "HI", 2 ) )
						sVal.Format( "%d", m_attackBase+m_attackRange );
				}
				else
				{
					sVal.Format( "%d,%d", m_attackBase, m_attackBase+m_attackRange );
				}
			} break;
		case OC_RANGE:
			{
				if ( RangeH() == 0 )
					sVal.Format( "%d", RangeL() );
				else
					sVal.Format( "%d,%d", RangeH(), RangeL() );
			}
			break;
		case OC_RANGEL:
			sVal.FormatVal( RangeH() );
			break;
		case OC_RANGEH:
			sVal.FormatVal( RangeL() );
			break;
		case OC_CAN:
			sVal.FormatHex( GetCanFlags() );
			break;
		case OC_CANMASK:
			sVal.FormatHex( m_CanMask );
			break;
		case OC_MODMAXWEIGHT:
			sVal.FormatVal(m_ModMaxWeight);
			return true;
		case OC_CANSEE:
		case OC_CANSEELOS:
		case OC_CANSEELOSFLAG:
			{
				bool bCanSee = ( index == OC_CANSEE );
				bool bFlags = ( index == OC_CANSEELOSFLAG );
				CChar *pChar = pSrc->GetChar();
				word flags = 0;

				pszKey += ( bCanSee ? 6 : (bFlags ? 13 : 9) );
				SKIP_SEPARATORS(pszKey);
				GETNONWHITESPACE(pszKey);

				if ( bFlags && *pszKey )
				{
					flags = Exp_GetWVal(pszKey);
					SKIP_ARGSEP(pszKey);
				}
				if ( *pszKey )		// has an argument - UID to see(los) or POS to los only
				{
					CPointMap pt;
					CUID uid;
					CObjBase *pObj = NULL;

					if ( !bCanSee )
						pt = g_Cfg.GetRegionPoint(pszKey);

					if ( bCanSee || !pt.IsValidPoint() )
					{
						uid = Exp_GetDWVal( pszKey );
						pObj = uid.ObjFind();
						if ( !bCanSee && pObj )
							pt = pObj->GetTopPoint();
					}

					pChar = GetUID().CharFind();

					if ( pChar )
						sVal.FormatVal(bCanSee ? pChar->CanSee(pObj) : pChar->CanSeeLOS(pt, NULL, pChar->GetVisualRange(), flags));
					else
						sVal.FormatVal(0);
				}
				else if ( !pChar )		// no char -> no see
					sVal.FormatVal(0);
				else					// standart way src TO current object
					sVal.FormatVal(bCanSee ? pChar->CanSee(this) : pChar->CanSeeLOS(this, (word)(flags)));
			}
			break;
		case OC_COLOR:
			sVal.FormatHex( GetHue() );
			break;
		case OC_COMPLEXITY:
			{
				if ( IsDisconnected() || !GetTopLevelObj()->GetTopPoint().IsValidPoint() )
					return false;
				return GetTopLevelObj()->GetTopSector()->r_WriteVal( pszKey, sVal, pSrc );
			}
		case OC_CTAGCOUNT:
			{
				CChar * pChar = dynamic_cast<CChar*>(this);
				if ( !pChar )
					sVal.FormatVal( 0 );
				else
					sVal.FormatSTVal( pChar->IsClient() ? (pChar->GetClient()->m_TagDefs.GetCount()) : 0 );
			}
			break;
		case OC_TEXTF:
			{
				tchar * key = const_cast<tchar*>(pszKey);
				key += 5;
				tchar * pszArg[4];
				int iArgQty = Str_ParseCmds(key, pszArg, CountOf(pszArg));
				if (iArgQty < 2)
				{
					g_Log.EventError("SysMessagef with less than 1 args for the given text\n");
					return false;
				}
				if (iArgQty > 4)
				{
					g_Log.EventError("Too many arguments given to SysMessagef (max = text + 3\n");
					return false;
				}
				//strip quotes if any
				if (*pszArg[0] == '"')
					++pszArg[0];
				byte count = 0;
				for (tchar * pEnd = pszArg[0] + strlen(pszArg[0]) - 1; pEnd >= pszArg[0]; --pEnd)
				{
					if (*pEnd == '"')
					{
						*pEnd = '\0';
						break;
					}
					++count;
				}
				sVal.Format(pszArg[0], pszArg[1], pszArg[2] ? pszArg[2] : 0, pszArg[3] ? pszArg[3] : 0);
				return true;
			}break;
		case OC_DIALOGLIST:
			{
				pszKey += 10;
				if ( *pszKey == '.' )
				{
					SKIP_SEPARATORS( pszKey );
					GETNONWHITESPACE( pszKey );

					CClient * pThisClient = pSrc->GetChar() ? ( pSrc->GetChar()->IsClient() ? pSrc->GetChar()->GetClient() : NULL ) : NULL;
					sVal.FormatVal(0);

					if ( pThisClient )
					{
						if( !strnicmp(pszKey, "COUNT", 5) )
						{
							sVal.FormatSTVal( pThisClient->m_mapOpenedGumps.size() );
						}
						else
						{
							CClient::OpenedGumpsMap_t * ourMap = &(pThisClient->m_mapOpenedGumps);
							size_t iDialogIndex = (size_t)( Exp_GetVal(pszKey) );
							SKIP_SEPARATORS(pszKey);

							if ( iDialogIndex <= ourMap->size() )
							{
								CClient::OpenedGumpsMap_t::iterator itGumpFound = ourMap->begin();
								while ( iDialogIndex-- )
									++itGumpFound;

								if ( !strnicmp(pszKey, "ID", 2) )
								{
									sVal.Format("%s", g_Cfg.ResourceGetName( CResourceID(RES_DIALOG, (*itGumpFound).first )) );
								}
								else if ( !strnicmp(pszKey, "COUNT", 5) )
								{
									sVal.FormatVal( (*itGumpFound).second );
								}
							}
						}
					}
					else
					{
						DEBUG_ERR(( "DIALOGLIST called on non-client object.\n" ));
					}

					return true;
				}
				else
					return false;
			}
			break;
		case OC_DISTANCE:
			{
				pszKey += 8;
				SKIP_SEPARATORS( pszKey );
				GETNONWHITESPACE( pszKey );
				CObjBase * pObj = pSrc->GetChar();

				CObjBase * pThis = this;
				if ( !IsTopLevel() )
					pThis = dynamic_cast <CObjBase*>( GetTopLevelObj() );
				if ( !pThis )
					return false;

				if ( *pszKey )
				{
					CPointMap pt = g_Cfg.GetRegionPoint( pszKey );

					if ( pt.IsValidPoint() )
					{
						if ( !pThis->GetTopPoint().IsValidPoint() )
							return false;
						sVal.FormatVal( pThis->GetTopDist( pt ) );
						return true;
					}

					CUID uid = Exp_GetVal( pszKey );
					SKIP_SEPARATORS( pszKey );
                    GETNONWHITESPACE( pszKey );
					pObj = uid.ObjFind();
				}

				if ( pObj && !pObj->IsTopLevel() )
					pObj = dynamic_cast <CObjBase*>( pObj->GetTopLevelObj() );
				if ( !pObj )
					return false;
				sVal.FormatVal( pThis->GetDist( pObj ) );
			}
			break;
		case OC_EVENTS:
			m_OEvents.WriteResourceRefList( sVal );
			break;
		case OC_FACING:
			{
				pszKey += 6;
				SKIP_SEPARATORS(pszKey);
				GETNONWHITESPACE(pszKey);

				CObjBase * pObj = pSrc->GetChar();

				CObjBase * pThis = this;
				if (!IsTopLevel())
					pThis = dynamic_cast <CObjBase*>(GetTopLevelObj());
				if (!pThis)
					return false;

				if (*pszKey)
				{
					CPointMap pt = g_Cfg.GetRegionPoint(pszKey);

					if (pt.IsValidPoint())
					{
						if (!pThis->GetTopPoint().IsValidPoint())
							return false;
						else
							sVal.FormatVal(pThis->GetTopPoint().GetDir(pt));
						return true;
					}

					CUID uid = Exp_GetVal(pszKey);
					SKIP_SEPARATORS(pszKey);
                    GETNONWHITESPACE(pszKey);
					pObj = uid.ObjFind();
				}

				if (!pObj)
					return false;
				if (!pObj->IsTopLevel())
					pObj = dynamic_cast <CObjBase*>(pObj->GetTopLevelObj());

				sVal.FormatVal(pThis->GetDir(pObj));
				break;
			}
		case OC_ISCHAR:
			sVal.FormatVal( IsChar());
			break;
		case OC_ISEVENT:
			if ( pszKey[7] != '.' )
				return false;
			pszKey += 8;
			sVal = m_OEvents.ContainsResourceName(RES_EVENTS, pszKey) ? "1" : "0";
			return true;
		case OC_ISTEVENT:
			if ( pszKey[8] != '.' )
				return false;
			pszKey += 9;
			sVal = Base_GetDef()->m_TEvents.ContainsResourceName(RES_EVENTS, pszKey) ? "1" : "0";
			return true;
		case OC_ISITEM:
			sVal.FormatVal( IsItem());
			break;
		case OC_ISCONT:
			sVal.FormatVal( IsContainer());
			break;
		case OC_ISNEARTYPETOP:
		case OC_ISNEARTYPE:
			{
				bool fP = false;
				pszKey	+= ( index == OC_ISNEARTYPETOP ) ? 13 : 10;
				if ( !strnicmp( pszKey, ".P", 2 ) )
				{
					fP	= true;
					pszKey	+= 2;
				}
				SKIP_SEPARATORS( pszKey );
				SKIP_ARGSEP( pszKey );

				if ( !GetTopPoint().IsValidPoint() )
					sVal.FormatVal( 0 );
				else
				{
					int iType = g_Cfg.ResourceGetIndexType( RES_TYPEDEF, pszKey );
					int iDistance;
					bool bCheckMulti;

					SKIP_IDENTIFIERSTRING( pszKey );
					SKIP_SEPARATORS( pszKey );
					SKIP_ARGSEP( pszKey );

					if ( !*pszKey )
						iDistance	= 0;
					else
						iDistance	= Exp_GetVal( pszKey );

					if ( !*pszKey )
						bCheckMulti = false;
					else
						bCheckMulti = Exp_GetVal( pszKey ) != 0;

					if ( fP )
					{
						CPointMap pt = ( index == OC_ISNEARTYPETOP ) ? ( g_World.FindTypeNear_Top(GetTopPoint(), static_cast<IT_TYPE>(iType), iDistance ) ) : ( g_World.FindItemTypeNearby(GetTopPoint(), static_cast<IT_TYPE>(iType), iDistance, bCheckMulti ) );

						if ( !pt.IsValidPoint() )
							sVal.FormatVal( 0 );
						else
							sVal = pt.WriteUsed();
					}
					else
						sVal.FormatVal( ( index == OC_ISNEARTYPETOP ) ? ( g_World.IsTypeNear_Top(GetTopPoint(), static_cast<IT_TYPE>(iType), iDistance ) ) : ( g_World.IsItemTypeNear(GetTopPoint(), static_cast<IT_TYPE>(iType), iDistance, bCheckMulti ) ) );
				}
				return true;
			}
			break;
		case OC_ISPLAYER:
			{
				CChar * pChar = dynamic_cast<CChar*>(this);
				if ( pChar )
					sVal.FormatVal( (pChar->m_pPlayer == NULL) ? 0 : 1 );
				else
					sVal.FormatVal( 0 );
				return true;
			}
		case OC_ISDIALOGOPEN:
			{
				pszKey += 12;
				SKIP_SEPARATORS( pszKey );
				GETNONWHITESPACE( pszKey );
				CChar * pCharToCheck = dynamic_cast<CChar*>(this);
				CClient * pClientToCheck = (pCharToCheck && pCharToCheck->IsClient()) ? (pCharToCheck->GetClient()) : NULL ;

				if ( pClientToCheck )
				{
					CResourceID rid = g_Cfg.ResourceGetIDType( RES_DIALOG, pszKey );
					int context;

					if ( pClientToCheck->GetNetState()->isClientKR() )
					{
						context = g_Cfg.GetKRDialog( (dword)rid ) & 0x00FFFFFF;
					}
					else
					{
						context = ((dword)rid) & 0x00FFFFFF;
					}

					CClient::OpenedGumpsMap_t::iterator itGumpFound = pClientToCheck->m_mapOpenedGumps.find( context );

					if ( itGumpFound != pClientToCheck->m_mapOpenedGumps.end() )
					{
						sVal.FormatVal( (*itGumpFound).second );
					}
					else
					{
						sVal.FormatVal( 0 );
					}
				}
				else
				{
					sVal.FormatVal( 0 );
				}

				return true;
			}
		case OC_ISARMOR:
			{
			pszKey += 7;
			SKIP_SEPARATORS( pszKey );
			GETNONWHITESPACE( pszKey );
			CItem * pItem = NULL;
			if ( *pszKey )
			{
				tchar * pszArg = Str_GetTemp();
                strncpynull( pszArg, pszKey, strlen( pszKey ) + 1 );

				CUID uid = Exp_GetVal( pszKey );
				pItem = dynamic_cast<CItem*> (uid.ObjFind());
				if (pItem == NULL)
				{
					ITEMID_TYPE id = (ITEMID_TYPE)(g_Cfg.ResourceGetID(RES_ITEMDEF, const_cast<lpctstr &>(reinterpret_cast<lptstr &>(pszArg))).GetResIndex());
					const CItemBase * pItemDef = CItemBase::FindItemBase( id );
					if ( pItemDef != NULL )
					{
						sVal.FormatVal( CItemBase::IsTypeArmor( pItemDef->GetType() ) );
						break;
					}
				}
				sVal.FormatVal( (( pItem ) ? ( pItem->IsTypeArmor() ) : ( 0 )) );
				break;
			}
			pItem = dynamic_cast<CItem*> (this);
			sVal.FormatVal( (( pItem ) ? ( pItem->IsTypeArmor() ) : ( 0 )) );
			break;
			}
		case OC_ISTIMERF:
			{
				if ( pszKey[8] != '.' )
					return false;
				pszKey += 9;
				//sVal.FormatVal( (g_World.m_TimedFunctions.IsTimer(GetUID(),pszKey)) ? 1 : 0 );
				sVal.FormatVal( g_World.m_TimedFunctions.IsTimer(GetUID(),pszKey) );
				return true;
			}
			break;
		case OC_ISWEAPON:
			{
			pszKey += 8;
			SKIP_SEPARATORS( pszKey );
			GETNONWHITESPACE( pszKey );
			CItem * pItem = NULL;
			if ( *pszKey )
			{
				tchar * pszArg = Str_GetTemp();
				strncpynull( pszArg, pszKey, strlen( pszKey ) + 1 );

				CUID uid = Exp_GetVal( pszKey );
				pItem = dynamic_cast<CItem*> (uid.ObjFind());
				if ( pItem == NULL )
				{
					ITEMID_TYPE id = (ITEMID_TYPE)(g_Cfg.ResourceGetID(RES_ITEMDEF, const_cast<lpctstr &>(reinterpret_cast<lptstr &>(pszArg))).GetResIndex());
					const CItemBase * pItemDef = CItemBase::FindItemBase( id );
					if (pItemDef != NULL)
					{
						sVal.FormatVal( CItemBase::IsTypeWeapon( pItemDef->GetType() ) );
						break;
					}
				}
				sVal.FormatVal( (( pItem ) ? ( pItem->IsTypeWeapon() ) : ( 0 )) );
				break;
			}
			pItem = dynamic_cast<CItem*> (this);
			sVal.FormatVal( (( pItem ) ? ( pItem->IsTypeWeapon() ) : ( 0 )) );
			break;
			}
		case OC_MAP:
			sVal.FormatVal( GetUnkPoint().m_map);
			break;
		case OC_MODAR:
		case OC_MODAC:
			sVal.FormatVal( m_ModAr );
			break;
		case OC_NAME:
			sVal = GetName();
			break;
		case OC_P:
			if ( pszKey[1] == '.' )
				return GetUnkPoint().r_WriteVal( pszKey+2, sVal );
			sVal = GetUnkPoint().WriteUsed();
			break;
		case OC_TAG0:
			fZero = true;
			++pszKey;
		case OC_TAG:			// "TAG" = get/set a local tag.
			{
				if ( pszKey[3] != '.' )
					return false;
				pszKey += 4;

				CVarDefCont * pVarKey	= m_TagDefs.GetKey( pszKey );
				if ( !pVarKey )
					sVal = Base_GetDef()->m_TagDefs.GetKeyStr( pszKey, fZero );
				else
					sVal = pVarKey->GetValStr();
			}
			return true;
		case OC_TIMER:
			sVal.FormatLLVal( GetTimerAdjusted() );
			break;
		case OC_TIMERD:
			sVal.FormatLLVal( GetTimerDAdjusted() );
			break;
		case OC_TRIGGER:
			{
				pszKey += 7;
				GETNONWHITESPACE( pszKey );

				if ( *pszKey )
				{
					TRIGRET_TYPE trReturn = TRIGRET_RET_FALSE;
					m_CallingObjTrigger = m_RunningTrigger;
					bool bTrigReturn = CallPersonalTrigger(const_cast<tchar *>(pszKey), pSrc, trReturn,false);
					m_CallingObjTrigger = NULL;
					if ( bTrigReturn )
						sVal.FormatVal(trReturn);

					return bTrigReturn;
				}
			} return false;
		case OC_TOPOBJ:
			if ( pszKey[6] == '.' )
				return CScriptObj::r_WriteVal( pszKey, sVal, pSrc );
			sVal.FormatHex(GetTopLevelObj()->GetUID());
			break;
		case OC_UID:
			if ( pszKey[3] == '.' )
				return	CScriptObj::r_WriteVal( pszKey, sVal, pSrc );
		case OC_SERIAL:
			sVal.FormatHex( GetUID() );
			break;
		case OC_SPAWNITEM:
            if (GetSpawn())
            {
                if (pszKey[9] == '.')
                    return CScriptObj::r_WriteVal(pszKey, sVal, pSrc);
                sVal.FormatHex(GetSpawn()->GetLink()->GetUID());
            }
            else
            {
                sVal.FormatHex(0);
            }
			break;
		case OC_SEXTANTP:
			{
				pszKey += 8;
				SKIP_SEPARATORS( pszKey );
                GETNONWHITESPACE( pszKey );

				CPointMap pt;
				if ( *pszKey )
					pt = g_Cfg.GetRegionPoint( pszKey );
				else
					pt = this->GetUnkPoint();

				if ( !pt.IsValidPoint() )
					return false;

				sVal = g_Cfg.Calc_MaptoSextant(pt);
		} break;
		case OC_SPEED:
		{
			if ( !this->IsItem() )
				return false;
			CItem * pItem = static_cast<CItem*>(this);
			sVal.FormatVal( pItem->GetSpeed() );
		}	break;
		case OC_TIMESTAMP:
			sVal.FormatLLVal( GetTimeStamp().GetTimeRaw() );
			break;
		case OC_VERSION:
			sVal = SPHERE_VERSION;
			break;
		case OC_WEIGHT:
			sVal.FormatVal( GetWeight() );
			break;
		case OC_Z:
			sVal.FormatVal( GetUnkZ() );
			break;
		case OC_TAGAT:
			{
 				pszKey += 5;	// eat the 'TAGAT'
 				if ( *pszKey == '.' )	// do we have an argument?
 				{
 					SKIP_SEPARATORS( pszKey );
 					size_t iQty = (size_t)( Exp_GetVal( pszKey ) );
					if ( iQty >= m_TagDefs.GetCount() )
 						return false; // trying to get non-existant tag

 					const CVarDefCont * pTagAt = m_TagDefs.GetAt( iQty );
 					if ( !pTagAt )
 						return false; // trying to get non-existant tag

 					SKIP_SEPARATORS( pszKey );
 					if ( ! *pszKey )
 					{
 						sVal.Format("%s=%s", static_cast<lpctstr>(pTagAt->GetKey()), static_cast<lpctstr>(pTagAt->GetValStr()));
 						return true;
 					}
 					else if ( !strnicmp( pszKey, "KEY", 3 )) // key?
 					{
 						sVal = static_cast<lpctstr>(pTagAt->GetKey());
 						return true;
 					}
 					else if ( !strnicmp( pszKey, "VAL", 3 )) // val?
 					{
 						sVal = pTagAt->GetValStr();
 						return true;
 					}
 				}

			return false;
			}
			break;
		case OC_TAGCOUNT:
			sVal.FormatSTVal( m_TagDefs.GetCount() );
			break;
		case OC_PROPSAT:
			{
 				pszKey += 7;	// eat the 'TAGAT'
 				if ( *pszKey == '.' )	// do we have an argument?
 				{
 					SKIP_SEPARATORS( pszKey );
 					size_t iQty = (size_t)( Exp_GetVal( pszKey ) );
					if ( iQty >= m_BaseDefs.GetCount() )
 						return false; // trying to get non-existant tag

 					const CVarDefCont * pTagAt = m_BaseDefs.GetAt( iQty );
 					if ( !pTagAt )
 						return false; // trying to get non-existant tag

 					SKIP_SEPARATORS( pszKey );
 					if ( ! *pszKey )
 					{
 						sVal.Format("%s=%s", static_cast<lpctstr>(pTagAt->GetKey()), static_cast<lpctstr>(pTagAt->GetValStr()));
 						return true;
 					}
 					else if ( !strnicmp( pszKey, "KEY", 3 )) // key?
 					{
 						sVal = static_cast<lpctstr>(pTagAt->GetKey());
 						return true;
 					}
 					else if ( !strnicmp( pszKey, "VAL", 3 )) // val?
 					{
 						sVal = pTagAt->GetValStr();
 						return true;
 					}
 				}

			return false;
			}
			break;
		case OC_PROPSCOUNT:
			sVal.FormatSTVal( m_BaseDefs.GetCount() );
			break;
		default:
			return false;
	}
	return true;
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_KEYRET(pSrc);
	EXC_DEBUG_END;
	return false;
}

bool CObjBase::r_LoadVal( CScript & s )
{
	ADDTOCALLSTACK("CObjBase::r_LoadVal");
	// load the basic stuff.
	EXC_TRY("LoadVal");
	// we're using FindTableSorted so we must do this here.
	// Using FindTableHeadSorted instead would result in keywords
	// starting with "P" not working, for instance :)

	if ( s.IsKeyHead("TAG.", 4) )
	{
		bool fQuoted = false;
		m_TagDefs.SetStr(s.GetKey()+4, fQuoted, s.GetArgStr(&fQuoted), false);
		return true;
	}
	else if ( s.IsKeyHead("TAG0.", 5) )
	{
		bool fQuoted = false;
		m_TagDefs.SetStr(s.GetKey()+5, fQuoted, s.GetArgStr(&fQuoted), true);
		return true;
	}

	int index = FindTableSorted( s.GetKey(), sm_szLoadKeys, CountOf( sm_szLoadKeys )-1 );
	if ( index < 0 )
	{
        return CScriptObj::r_LoadVal(s);
	}

    bool fResendTooltip = false;
	switch ( index )
	{
		//Set as Strings
		case OC_HITSPELL:
		case OC_SLAYER:
		case OC_SLAYERLESSER:
		case OC_SLAYERMISC:
		case OC_SLAYERSUPER:
		case OC_ABILITYPRIMARY:
		case OC_ABILITYSECONDARY:
		case OC_MANABURST:
			{
				bool fQuoted = false;
				SetDefStr(s.GetKey(), s.GetArgStr( &fQuoted ), fQuoted);
                fResendTooltip = true;
			}
			break;
		//Set as number only
		case OC_INCREASEHITCHANCE:
		case OC_CASTINGFOCUS:
		case OC_DAMCHAOS:
		case OC_DAMCOLD:
		case OC_DAMDIRECT:
		case OC_DAMENERGY:
		case OC_DAMFIRE:
		case OC_DAMMODIFIER:
		case OC_DAMPHYSICAL:
		case OC_DAMPOISON:
		case OC_DECREASEHITCHANCE:
		case OC_EATERCOLD:
		case OC_EATERDAM:
		case OC_EATERENERGY:
		case OC_EATERFIRE:
		case OC_EATERKINETIC:
		case OC_EATERPOISON:
		case OC_ENHANCEPOTIONS:
		case OC_EXPANSION:
		case OC_HITAREACOLD:
		case OC_HITAREAENERGY:
		case OC_HITAREAFIRE:
		case OC_HITAREAPHYSICAL:
		case OC_HITAREAPOISON:
		case OC_HITCURSE:
		case OC_HITDISPEL:
		case OC_HITFATIGUE:
		case OC_HITFIREBALL:
		case OC_HITHARM:
		case OC_HITLEECHLIFE:
		case OC_HITLEECHMANA:
		case OC_HITLEECHSTAM:
		case OC_HITLIGHTNING:
		case OC_HITLOWERATK:
		case OC_HITLOWERDEF:
		case OC_HITMAGICARROW:
		case OC_HITMANADRAIN:
		case OC_HITSPELLSTR:
		case OC_INCREASEGOLD:
		case OC_INCREASEKARMALOSS:
		case OC_LOWERAMMOCOST:
		case OC_LOWERREQ:
		case OC_MANABURSTFREQUENCY:
		case OC_MANABURSTKARMA:
		case OC_NIGHTSIGHT:
		case OC_RAGEFOCUS:
		case OC_REACTIVEPARALYZE:
		case OC_REFLECTPHYSICALDAM:
		case OC_RESONANCECOLD:
		case OC_RESONANCEENERGY:
		case OC_RESONANCEFIRE:
		case OC_RESONANCEKINETIC:
		case OC_RESONANCEPOISON:
		case OC_SOULCHARGE:
		case OC_SOULCHARGECOLD:
		case OC_SOULCHARGEENERGY:
		case OC_SOULCHARGEFIRE:
		case OC_SOULCHARGEKINETIC:
		case OC_SOULCHARGEPOISON:
		case OC_SPELLCHANNELING:
		case OC_SPELLCONSUMPTION:
		case OC_SPELLFOCUSING:
		case OC_VELOCITY:
		case OC_NAMELOC:
        {
            SetDefNum(s.GetKey(), s.GetArgVal(), false);
            fResendTooltip = true;
            break;
        }
		case OC_INCREASESWINGSPEED:
		case OC_INCREASEDAM:
		case OC_LOWERREAGENTCOST:
		case OC_LOWERMANACOST:
		case OC_FASTERCASTRECOVERY:
		case OC_FASTERCASTING:
		case OC_INCREASEDEFCHANCE:
		case OC_INCREASEDEFCHANCEMAX:
		case OC_INCREASESPELLDAM:
		case OC_RESCOLDMAX:
		case OC_RESFIREMAX:
		case OC_RESENERGYMAX:
		case OC_RESPOISONMAX:
		case OC_RESPHYSICAL:
		case OC_RESPHYSICALMAX:
		case OC_RESFIRE:
		case OC_RESCOLD:
		case OC_RESPOISON:
		case OC_RESENERGY:
		case OC_LUCK:
		case OC_REGENFOOD:
		case OC_REGENHITS:
		case OC_REGENSTAM:
		case OC_REGENMANA:
		case OC_REGENVALFOOD:
		case OC_REGENVALHITS:
		case OC_REGENVALSTAM:
		case OC_REGENVALMANA:
		case OC_COMBATBONUSSTAT:
		case OC_COMBATBONUSPERCENT:
			{
				SetDefNum(s.GetKey(),s.GetArgVal());

				// This should be used in case items with these properties updates the character in the moment without any script to make status reflect the update.
				// Maybe too a cliver check to not send update if not needed.
				if (IsSetCombatFlags(COMBAT_ELEMENTAL_ENGINE))
				{
					CChar * pChar = dynamic_cast <CChar*>(GetTopLevelObj());
					if (pChar)
						pChar->UpdateStatsFlag();
				}
                fResendTooltip = true;
				break;
			}
        case OC_RECIPEALCHEMY:
        case OC_RECIPEBLACKSMITH:
        case OC_RECIPEBOWCRAFT:
        case OC_RECIPECARPENTRY:
        case OC_RECIPECARTOPGRAHY:
        case OC_RECIPECOOKING:
        case OC_RECIPEGLASSBLOWING:
        case OC_RECIPEINSCRIPTION:
        case OC_RECIPEMASONRY:
        case OC_RECIPETAILORING:
        case OC_RECIPETINKERING:
        {
            SetDefNum(s.GetKey(), s.GetArgLLVal());
            fResendTooltip = true;
            break;
        }
		case OC_ARMOR:
			{
				if ( IsChar() )
					return false;

				int64 piVal[2];
				size_t iQty = Str_ParseCmds( s.GetArgStr(), piVal, CountOf(piVal));
				m_defenseBase = (uchar)(piVal[0]);
				if ( iQty > 1 )
					m_defenseRange = (uchar)(piVal[1]) - m_defenseBase;
				else
					m_defenseRange = 0;
				CChar * pChar = dynamic_cast <CChar*>(GetTopLevelObj());
				if ( pChar )
					pChar->UpdateStatsFlag();
                fResendTooltip = true;
				break;
			}
		case OC_DAM:
			{
				int64 piVal[2];
				size_t iQty = Str_ParseCmds( s.GetArgStr(), piVal, CountOf(piVal));
				m_attackBase = (uchar)(piVal[0]);
				if ( iQty > 1 )
					m_attackRange = (uchar)(piVal[1]) - m_attackBase;
				else
					m_attackRange = 0;
				CChar * pChar = dynamic_cast <CChar*>(GetTopLevelObj());
				if (pChar)
					pChar->UpdateStatsFlag();
                fResendTooltip = true;
			}
			break;
		case OC_WEIGHTREDUCTION:
			{
				int oldweight = GetWeight();
				SetDefNum(s.GetKey(),s.GetArgVal(), false);
				CContainer * pCont = dynamic_cast <CContainer*> (GetParent());
				if (pCont)
				{
					ASSERT( IsItemEquipped() || IsItemInContainer());
					pCont->OnWeightChange(GetWeight() - oldweight);
				}
                fResendTooltip = true;
			}
			return true;

		case OC_RANGE:
			{
				int64 piVal[2];
				size_t iQty = Str_ParseCmds( s.GetArgStr(), piVal, CountOf(piVal));
				if ( iQty > 1 )
				{
					int64 iRange = ((piVal[0] & 0xff) << 8) & 0xff00;
					iRange |= (piVal[1] & 0xff);
					SetDefNum(s.GetKey(),iRange, false);
				}
				else
				{
					SetDefNum(s.GetKey(),piVal[0], false);
				}
                fResendTooltip = true;
			}
			break;

		case OC_CAN:
			return false;
		case OC_CANMASK:
        {
            dword dwFlags = s.GetArgDWVal();
            m_CanMask = dwFlags;
            if (IsItem())
            {
                g_Log.EventDebug("CObjBase::r_LoadVal(OC_CANMASK)\n");
                CCItemDamageable *pItemDmg = static_cast<CCItemDamageable*>(GetComponent(COMP_ITEMDAMAGEABLE));
                if ((dwFlags & CAN_I_DAMAGEABLE) && !pItemDmg)
                {
                    g_Log.EventDebug("CObjBase::r_LoadVal(OC_CANMASK) 1\n");
                    Suscribe(new CCItemDamageable(this));
                }
                else if (!(dwFlags & CAN_I_DAMAGEABLE) && pItemDmg)
                {
                    g_Log.EventDebug("CObjBase::r_LoadVal(OC_CANMASK) 2\n");
                    Unsuscribe(pItemDmg);
                }
                Update();   // Required to force the client to allow dragging the item's bar or to do not allow it anymore before trying to do it.
            }
            break;
        }
		case OC_MODMAXWEIGHT:
			m_ModMaxWeight = s.GetArgVal();
            fResendTooltip = true;
			break;
		case OC_COLOR:
			if ( !strnicmp( s.GetArgStr(), "match_shirt", 11 ) || !strnicmp( s.GetArgStr(), "match_hair", 10 ))
			{
				CChar * pChar = dynamic_cast <CChar*>(GetTopLevelObj());
				if ( pChar )
				{
					CItem * pHair = pChar->LayerFind( !strnicmp( s.GetArgStr()+6, "shirt",5 ) ? LAYER_SHIRT : LAYER_HAIR );
					if ( pHair )
					{
						m_wHue = pHair->GetHue();
						break;
					}
				}
				m_wHue = HUE_GRAY;
				break;
			}
			RemoveFromView();
			SetHue((HUE_TYPE)(s.GetArgVal()), false, &g_Serv); //@Dye is called from @Create/.xcolor/script command here // since we can not receive pSrc on this r_LoadVal function ARGO/SRC will be null
			Update();
			break;
		case OC_EVENTS:
			return( m_OEvents.r_LoadVal( s, RES_EVENTS ));
		case OC_MAP:
			// Move to another map
			if ( ! IsTopLevel())
				return false;
			{
				CPointMap pt = GetTopPoint();
				pt.m_map = (uchar)(s.GetArgVal());

				//	is the desired mapplane allowed?
				if ( !g_MapList.m_maps[pt.m_map] )
					return false;

				MoveTo(pt);
				if (IsItem())
					Update();
			}
			break;
		case OC_MODAR:
		case OC_MODAC:
			{
				m_ModAr = s.GetArgVal();
				CChar * pChar = dynamic_cast <CChar*>(GetTopLevelObj());
				if ( pChar && pChar->IsChar() )
				{
					pChar->m_defense = (word)(pChar->CalcArmorDefense());
					pChar->UpdateStatsFlag();
				}
                fResendTooltip = true;
			}
			break;
		case OC_NAME:
			SetName( s.GetArgStr());
            fResendTooltip = true;
			break;
		case OC_P:	// Must set the point via the CItem or CChar methods.
			return false;
		case OC_SPEED:
		{
			if (!this->IsItem())
				return false;
			CItem * pItem = static_cast<CItem*>(this);
			pItem->m_speed = (byte)(s.GetArgVal());
			pItem->ResendTooltip();
			return true;
		}
		case OC_TIMER:
			SetTimeout( s.GetArgLLVal() * TICK_PER_SEC );
            fResendTooltip = true;
			break;
		case OC_TIMERD:
			SetTimeout( s.GetArgLLVal());
            fResendTooltip = true;
			break;
		case OC_TIMESTAMP:
			SetTimeStamp( s.GetArgLLVal());
			break;
		case OC_SPAWNITEM:
        {
            if ( !g_Serv.IsLoading() )	// SPAWNITEM is read-only
                return false;
            _uidSpawn = s.GetArgDWVal();
            break;
        }
		case OC_UID:
		case OC_SERIAL:
			// Don't set container flags through this.
			SetUID( s.GetArgVal(), (dynamic_cast <CItem*>(this)) ? true : false );
			break;
		default:
			return false;
	}
    if (fResendTooltip)
    {
        ResendTooltip();
    }
	return true;
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_SCRIPT;
	EXC_DEBUG_END;
	return false;
}

void CObjBase::r_Write( CScript & s )
{
	ADDTOCALLSTACK_INTENSIVE("CObjBase::r_Write");
	s.WriteKeyHex( "SERIAL", GetUID());
	if ( IsIndividualName() )
		s.WriteKey( "NAME", GetIndividualName());
	if ( m_wHue != HUE_DEFAULT )
		s.WriteKeyHex( "COLOR", GetHue());
	if ( m_timeout.IsTimeValid() )
		s.WriteKeyVal( "TIMER", GetTimerAdjusted());
	if ( m_timestamp.IsTimeValid() )
		s.WriteKeyVal( "TIMESTAMP", GetTimeStamp().GetTimeRaw());
	if ( GetSpawn() )
		s.WriteKeyHex("SPAWNITEM", GetSpawn()->GetLink()->GetUID());
	if ( m_ModAr )
		s.WriteKeyVal("MODAR", m_ModAr);
	if ( m_ModMaxWeight )
		s.WriteKeyVal("MODMAXWEIGHT", m_ModMaxWeight);

	// Write New variables
	m_BaseDefs.r_WritePrefix(s);

	m_TagDefs.r_WritePrefix(s, "TAG");
	m_OEvents.r_Write(s, "EVENTS");
}

enum OV_TYPE
{
	#define ADD(a,b) OV_##a,
	#include "../tables/CObjBase_functions.tbl"
	#undef ADD
	OV_QTY
};

lpctstr const CObjBase::sm_szVerbKeys[OV_QTY+1] =
{
	#define ADD(a,b) b,
	#include "../tables/CObjBase_functions.tbl"
	#undef ADD
	NULL
};

bool CObjBase::r_Verb( CScript & s, CTextConsole * pSrc ) // Execute command from script
{
	ADDTOCALLSTACK("CObjBase::r_Verb");
	EXC_TRY("Verb");
	lpctstr	pszKey = s.GetKey();
	ASSERT(pSrc);
	int	index;

	if ( !strnicmp(pszKey, "CLEARTAGS", 9) )
	{
		pszKey = s.GetArgStr();
		SKIP_SEPARATORS(pszKey);
		m_TagDefs.ClearKeys(pszKey);
		return true;
	}

	CSString sVal;
	CScriptTriggerArgs Args( s.GetArgRaw() );
	if ( r_Call( pszKey, pSrc, &Args, &sVal ) )
		return true;

	if ( !strnicmp( pszKey, "TARGET", 6 ) )
		index = OV_TARGET;
	else
		index = FindTableSorted( pszKey, sm_szVerbKeys, CountOf(sm_szVerbKeys)-1 );
    if (index < 0)
    {
        return CScriptObj::r_Verb(s, pSrc);
    }

	CChar * pCharSrc = pSrc->GetChar();
	CClient * pClientSrc = (pCharSrc && pCharSrc->IsClient()) ? (pCharSrc->GetClient()) : nullptr ;

	switch (index)
	{
        case OV_ADDCLILOC:
            // Add cliloc in @ClientTooltip trigger
            {
                tchar * ppLocArgs[256];
                int qty = Str_ParseCmds(s.GetArgRaw(), ppLocArgs, CountOf(ppLocArgs), ",");
                dword clilocid = Exp_GetVal(ppLocArgs[0]);

                CSString sLocArgs;
                for (int y = 1 ; y < qty; ++y )
                {
                    if ( sLocArgs.GetLength() )
                        sLocArgs += "\t";
                    sLocArgs += ( !strncmp(ppLocArgs[y], "NULL", 4) ? " " : ppLocArgs[y] );
                }

                if ( g_Cfg.m_iDebugFlags & DEBUGF_SCRIPTS )
                    g_Log.EventDebug("SCRIPT: addcliloc(%u,'%s')\n", clilocid, sLocArgs.GetPtr());
                m_TooltipData.emplace_back(new CClientTooltip(clilocid, sLocArgs.GetPtr()));
            }
            break;
		case OV_DAMAGE:	//	"Dmg, SourceFlags, SourceCharUid, DmgPhysical(%), DmgFire(%), DmgCold(%), DmgPoison(%), DmgEnergy(%)" = do me some damage.
			{
				EXC_SET("DAMAGE");
				int64 piCmd[8];
				int iArgQty = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd));
				if ( iArgQty < 1 )
					return false;
				if ( iArgQty > 2 )	// Give it a new source char UID
				{
					CObjBaseTemplate * pObj = CUID( (dword)(piCmd[2]) ).ObjFind();
					if ( pObj )
						pObj = pObj->GetTopLevelObj();
					pCharSrc = dynamic_cast<CChar*>(pObj);
				}

				CChar *pChar = dynamic_cast<CChar *>(this);
				CItem *pItem = dynamic_cast<CItem *>(this);
				if ( pChar )
					pChar->OnTakeDamage((int)(piCmd[0]),
						pCharSrc,
						(iArgQty >= 1) ? (DAMAGE_TYPE)(piCmd[1]) : DAMAGE_HIT_BLUNT|DAMAGE_GENERAL,
						(iArgQty >= 3) ? (int)(piCmd[3]) : 0,		// physical damage %
						(iArgQty >= 4) ? (int)(piCmd[4]) : 0,		// fire damage %
						(iArgQty >= 5) ? (int)(piCmd[5]) : 0,		// cold damage %
						(iArgQty >= 6) ? (int)(piCmd[6]) : 0,		// poison damage %
						(iArgQty >= 7) ? (int)(piCmd[7]) : 0			// energy damage %
					);
				else if ( pItem )
					pItem->OnTakeDamage((int)(piCmd[0]),
						pCharSrc,
						(iArgQty >= 1) ? (DAMAGE_TYPE)(piCmd[1]) : DAMAGE_HIT_BLUNT|DAMAGE_GENERAL
					);
			}
			break;

		case OV_EDIT:
			{
				EXC_SET("EDIT");
				// Put up a list of items in the container. (if it is a container)
				if ( pClientSrc == NULL )
					return false;
				pClientSrc->m_Targ_Text = s.GetArgStr();
				pClientSrc->Cmd_EditItem( this, -1 );
			}
			break;
		case OV_EFFECT: // some visual effect.
			{
				EXC_SET("EFFECT");
				int64 piCmd[12];
				int iArgQty = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd) );
				if ( iArgQty < 2 )
					return false;
				CObjBase *	pThis	= this;
				//DEBUG_ERR(("this->GetUID() 0%x \n", (dword)this->GetUID()));
				if ( piCmd[0] == -1 )
				{
					if ( pCharSrc )
					{
						piCmd[0]	= EFFECT_BOLT;
						pThis		= pCharSrc;
						pCharSrc	= dynamic_cast <CChar*>(this);
					}

				}
				//DEBUG_ERR(("this->GetUID() 0%x pThis->GetUID() 0%x pCharSrc->GetUID() 0%x\n",(dword)this->GetUID(),(dword)pThis->GetUID(),(dword)pCharSrc->GetUID()));
				pThis->Effect( static_cast<EFFECT_TYPE>(piCmd[0]), (ITEMID_TYPE)(RES_GET_INDEX(piCmd[1]) ),
					pCharSrc,
					(iArgQty >= 3)? (uchar)(piCmd[2]) : 5,		// byte bSpeedSeconds = 5,
					(iArgQty >= 4)? (uchar)(piCmd[3]) : 1,		// byte bLoop = 1,
					(iArgQty >= 5)? (piCmd[4] != 0) : false,	// bool fExplode = false
					(iArgQty >= 6)? (uint)(piCmd[5]) : 0,		// hue
					(iArgQty >= 7)? (uint)(piCmd[6]) : 0,		// render mode,
					(iArgQty >= 8) ? (word)(piCmd[7]) : 0,		// EffectID	//New Packet 0xc7
					(iArgQty >= 9) ? (word)(piCmd[8]) : 0,		// ExplodeID
					(iArgQty >= 10) ? (word)(piCmd[9]) : 0,		// ExplodeSound
					(iArgQty >= 11) ? (dword)(piCmd[10]) : 0,	// EffectUID
					(iArgQty >= 12) ? (uchar)(piCmd[11]) : 0	// Type
					);
			}
			break;
		case OV_EFFECTLOCATION: // visual effect at map point.
		{
			EXC_SET("EFFECTLOCATION");
			int64 piCmd[15];
			int iArgQty = Str_ParseCmds(s.GetArgStr(), piCmd, CountOf(piCmd));
			if (iArgQty < 2)
				return false;
			CObjBase *pThis = this;

			CPointMap pt((short)piCmd[0], (short)piCmd[1], (char)piCmd[2]);
			if (!pt.IsValidPoint())
				return false;

			//DEBUG_ERR(("this->GetUID() 0%x \n", (dword)this->GetUID()));
			if (piCmd[3] == -1)
			{
				if (pCharSrc)
				{
					piCmd[3] = EFFECT_BOLT;
					pThis = pCharSrc;
					pCharSrc = dynamic_cast <CChar*>(this);
				}

			}
			//DEBUG_ERR(("this->GetUID() 0%x pThis->GetUID() 0%x pCharSrc->GetUID() 0%x\n",(dword)this->GetUID(),(dword)pThis->GetUID(),(dword)pCharSrc->GetUID()));
			pThis->Effect(static_cast<EFFECT_TYPE>(piCmd[3]), (ITEMID_TYPE)(RES_GET_INDEX(piCmd[4])),
				pt,
				pCharSrc,
				(iArgQty >= 3) ? (uchar)(piCmd[5]) : 5,		// byte bSpeedSeconds = 5,
				(iArgQty >= 4) ? (uchar)(piCmd[6]) : 1,		// byte bLoop = 1,
				(iArgQty >= 5) ? (piCmd[7] != 0) : false,	// bool fExplode = false
				(iArgQty >= 6) ? (uint)(piCmd[8]) : 0,		// hue
				(iArgQty >= 7) ? (uint)(piCmd[9]) : 0,		// render mode,
				(iArgQty >= 8) ? (word)(piCmd[10]) : 0,		// EffectID	//New Packet 0xc7
				(iArgQty >= 9) ? (word)(piCmd[11]) : 0,		// ExplodeID
				(iArgQty >= 10) ? (word)(piCmd[12]) : 0,	// ExplodeSound
				(iArgQty >= 11) ? (dword)(piCmd[13]) : 0,	// EffectUID
				(iArgQty >= 12) ? (uchar)(piCmd[14]) : 0	// Type
			);
		}
		break;
		case OV_EMOTE:
			EXC_SET("EMOTE");
			Emote( s.GetArgStr() );
			break;
		case OV_FLIP:
			EXC_SET("FLIP");
			Flip();
			break;
		case OV_INPDLG:
			// "INPDLG" verb maxchars
			// else assume it was a property button.
			{
				EXC_SET("INPDLG");
				if ( pClientSrc == NULL )
					return false;

				tchar *Arg_ppCmd[2];		// Maximum parameters in one line
				size_t iQty = Str_ParseCmds( s.GetArgStr(), Arg_ppCmd, CountOf( Arg_ppCmd ));

				CSString sOrgValue;
				if ( ! r_WriteVal( Arg_ppCmd[0], sOrgValue, pSrc ))
					sOrgValue = ".";

				pClientSrc->m_Targ_Text = Arg_ppCmd[0];	// The attribute we want to edit.

				int iMaxLength = iQty > 1 ? ATOI(Arg_ppCmd[1]) : 1;

				CSString sPrompt;
				sPrompt.Format("%s (# = default)", static_cast<lpctstr>(Arg_ppCmd[0]));
				pClientSrc->addGumpInpVal( true, INPVAL_STYLE_TEXTEDIT,
					iMaxLength,	sPrompt, sOrgValue, this );
			}
			break;
		
		case OV_MENU:
			{
				EXC_SET("MENU");
				if ( pClientSrc == NULL )
					return false;
				pClientSrc->Menu_Setup( g_Cfg.ResourceGetIDType( RES_MENU, s.GetArgStr()), this );
			}
			break;
		case OV_MESSAGE:	//put info message (for pSrc client only) over item.
		case OV_MSG:
			{
				EXC_SET("MESSAGE or MSG");
				if ( pCharSrc == NULL )
					UpdateObjMessage(s.GetArgStr(), s.GetArgStr(), NULL, HUE_TEXT_DEF, TALKMODE_OBJ);
				else
					pCharSrc->ObjMessage(s.GetArgStr(), this);
			}
			break;
		case OV_MESSAGEUA:
			{
				EXC_SET("MESSAGEUA");
				if ( pClientSrc == NULL )
					break;	// We show the message only to players

				tchar * pszArgs[5];
				nchar ncBuffer[ MAX_TALK_BUFFER ];

				int iArgQty = Str_ParseCmds( s.GetArgRaw(), pszArgs, CountOf(pszArgs) );
				if ( iArgQty < 5 )
					break;

				CvtSystemToNUNICODE( ncBuffer, CountOf( ncBuffer ), pszArgs[4], -1 );
				pClientSrc->addBarkUNICODE( ncBuffer, this,
					(HUE_TYPE)( pszArgs[0][0] ? Exp_GetVal(pszArgs[0]) : HUE_TEXT_DEF ),
					static_cast<TALKMODE_TYPE>( pszArgs[1][0] ? Exp_GetVal(pszArgs[1]) : TALKMODE_SAY ),
					(FONT_TYPE)( pszArgs[2][0] ? Exp_GetVal(pszArgs[2]) : FONT_NORMAL ),
					CLanguageID(pszArgs[3]));
				break;
			}
		case OV_MOVE:
			// move without restriction. east,west,etc. (?up,down,)
			EXC_SET("MOVE");
			if ( IsTopLevel())
			{
				CPointMap pt = GetTopPoint();
				if ( ! GetDeltaStr( pt, s.GetArgStr()))
					return false;
				MoveTo( pt );
				Update();
			}
			break;
		case OV_MOVENEAR:
			{
				EXC_SET("MOVENEAR");
				CObjBase *	pObjNear;
				int64 piCmd[4];

				int iArgQty = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd) );
				if ( iArgQty <= 0 )
					return false;
				if ( iArgQty < 3 )
					piCmd[2] = 1;
				if ( iArgQty < 2 )
					piCmd[1] = 1;

				CUID uid = (dword)(piCmd[0]);
				pObjNear = uid.ObjFind();
				if ( !pObjNear )
					return false;
				MoveNearObj( pObjNear, (word)(piCmd[1]) );
				if ( piCmd[2] )
					Update();
			}
			break;
		case OV_NUDGEDOWN:
			EXC_SET("NUDGEDOWN");
			if ( IsTopLevel())
			{
				char zdiff = (char)(s.GetArgVal());
				SetTopZ( GetTopZ() - ( zdiff ? zdiff : 1 ));
				Update();
			}
			break;
		case OV_NUDGEUP:
			EXC_SET("NUDGEUP");
			if ( IsTopLevel())
			{
				char zdiff = (char)(s.GetArgVal());
				SetTopZ( GetTopZ() + ( zdiff ? zdiff : 1 ));
				Update();
			}
			break;
		case OV_MOVETO:
		case OV_P:
        {
			EXC_SET("P or MOVETO");
            CPointMap pt( g_Cfg.GetRegionPoint(s.GetArgStr()) );
            if (pt.IsValidPoint())
            {
                RemoveFromView();
                MoveTo(pt);
                Update();
            }
            else
                return false;
        }	
			break;
		case OV_PROMPTCONSOLE:
		case OV_PROMPTCONSOLEU:
			{
				EXC_SET("PROMPTCONSOLE/U");
				if ( pClientSrc == NULL )
					return false;

				tchar * pszArgs[2];

				int iArgQty = Str_ParseCmds( s.GetArgRaw(), pszArgs, CountOf(pszArgs) );
				if ( iArgQty == 0 )
					break;

				pClientSrc->addPromptConsoleFunction( pszArgs[0], pszArgs[1], (index == OV_PROMPTCONSOLEU) );
			}
			break;
		case OV_INFO:
			EXC_SET("INFO");
			if ( ! pClientSrc )
				return false;
			return pClientSrc->addGumpDialogProps( GetUID() );
		case OV_REMOVE:	//remove this object now.
			EXC_SET("REMOVE");
			Delete();
			return true;
		case OV_REMOVEFROMVIEW:
			EXC_SET("REMOVEFROMVIEW");
			RemoveFromView( NULL, false );	// remove this item from all clients.
			return true;
		case OV_RESENDTOOLTIP:
			{
				EXC_SET("RESENDTOOLTIP");

				int64 piCmd[2];
				int iArgQty = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd) );

				bool bSendFull = false;
				bool bUseCache = false;

				if (iArgQty >= 1)
					bSendFull = (piCmd[0] != 0);
				if (iArgQty >= 2)
					bUseCache = (piCmd[1] != 0);

				ResendTooltip(bSendFull, bUseCache);
				return true;
			}
		case OV_SAY: //speak so everyone can here
			EXC_SET("SAY");
			Speak( s.GetArgStr() );
			break;

		case OV_SAYU:
			// Speak in unicode from the UTF8 system format.
			EXC_SET("SAYU");
			SpeakUTF8( s.GetArgStr(), HUE_TEXT_DEF, TALKMODE_SAY, FONT_NORMAL );
			break;

		case OV_SAYUA:
			// This can have full args. SAYUA Color, Mode, Font, Lang, Text Text
			{
				EXC_SET("SAYUA");
				tchar * pszArgs[5];
				int iArgQty = Str_ParseCmds( s.GetArgRaw(), pszArgs, CountOf(pszArgs) );
				if ( iArgQty < 5 )
					break;

				SpeakUTF8( pszArgs[4],
					(HUE_TYPE)( pszArgs[0][0] ? Exp_GetVal(pszArgs[0]) : HUE_TEXT_DEF ),
					static_cast<TALKMODE_TYPE>( pszArgs[1][0] ? Exp_GetVal(pszArgs[1]) : TALKMODE_SAY ),
					(FONT_TYPE)( pszArgs[2][0] ? Exp_GetVal(pszArgs[2]) : FONT_NORMAL ),
					CLanguageID(pszArgs[3]));
			}
			break;

		case OV_SOUND:
			{
				EXC_SET("SOUND");
				int64 piCmd[2];
				int iArgQty = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd));
				if (!iArgQty)
					return false;
				Sound( (SOUND_TYPE)(piCmd[0]), ( iArgQty > 1 ) ? (int)(piCmd[1]) : 1 );
			}
			break;
		case OV_SPELLEFFECT:	// spell, strength, noresist
			{
				EXC_SET("SPELLEFFECT");
				int64 piCmd[4];
				size_t iArgs = Str_ParseCmds( s.GetArgStr(), piCmd, CountOf(piCmd));
				CItem * pItemSrc = NULL;
				switch( iArgs )
				{
				case 4:
					{
						CUID uid = (dword) piCmd[3];
						pItemSrc = uid.ItemFind();
					}
				case 3:
					if ( piCmd[2] == -1 )
					{
						pCharSrc = dynamic_cast <CChar*> (this);
					}
					else
					{
						CUID uid = (dword) piCmd[2];
						pCharSrc = uid.CharFind();
					}
					break;
				default:
					break;
				}
				OnSpellEffect((SPELL_TYPE)(RES_GET_INDEX(piCmd[0])), pCharSrc, (int)(piCmd[1]), pItemSrc);
			}
			break;
		case OV_TAGLIST:
			{
				EXC_SET("TAGLIST");
				if ( ! strcmpi( s.GetArgStr(), "log" ))
					pSrc = &g_Serv;
				m_TagDefs.DumpKeys(pSrc, "TAG.");
			}break;

		case OC_PROPSLIST:
			{
				EXC_SET("PROPSLIST");
				if ( ! strcmpi( s.GetArgStr(), "log" ))
					pSrc = &g_Serv;
				m_BaseDefs.DumpKeys(pSrc, nullptr);
			}break;

		case OV_TARGET:
			{
				EXC_SET("TARGET");
				if ( !pClientSrc )
					return false;
				pszKey	+= 6;
				bool fAllowGround = false;
				bool fCheckCrime = false;
				bool fFunction = false;
				bool fMulti	= false;
				tchar low = (tchar)(tolower(*pszKey));

				while (( low >= 'a' ) && ( low <= 'z' ))
				{
					if ( low == 'f' )
						fFunction = true;
					else if ( low == 'g' )
						fAllowGround = true;
					else if ( low == 'w' )
						fCheckCrime = true;
					else if ( low == 'm' )
						fMulti = true;

					low = (tchar)(tolower(*(++pszKey)));
				}

				pClientSrc->m_Targ_UID = GetUID();
				pClientSrc->m_tmUseItem.m_pParent = GetParent();	// Cheat Verify

				if ( fFunction )
				{
					if ( fMulti )
					{
						if ( IsStrEmpty(s.GetArgStr()) )
							break;
						char * ppArg[3];
						Str_ParseCmds( s.GetArgStr(), ppArg, CountOf(ppArg), "," );
						if ( !IsStrNumeric( ppArg[1] ))
							DEBUG_ERR(("Invalid argument in Target Multi\n"));
						ITEMID_TYPE itemid = (ITEMID_TYPE)(Exp_GetVal(ppArg[1]));
                        HUE_TYPE color = (HUE_TYPE)(Exp_GetVal(ppArg[2]));
						pClientSrc->addTargetFunctionMulti( ppArg[0], itemid, color, fAllowGround );
					}
					else
						pClientSrc->addTargetFunction( s.GetArgStr(), fAllowGround, fCheckCrime );
				}
				else
				{
					if ( fMulti )
					{
                        char *ppArg[2];
                        Str_ParseCmds(s.GetArgStr(), ppArg, CountOf(ppArg), ",");
						if ( !IsStrNumeric(ppArg[0]))
							DEBUG_ERR(("Invalid argument in Target Multi\n"));
						ITEMID_TYPE itemid = (ITEMID_TYPE)(Exp_GetVal(ppArg[0]));
                        HUE_TYPE color = (HUE_TYPE)(Exp_GetVal(ppArg[1]));
						pClientSrc->addTargetItems( CLIMODE_TARG_USE_ITEM, itemid, color, fAllowGround );
					}
					else
						pClientSrc->addTarget( CLIMODE_TARG_USE_ITEM, s.GetArgStr(), fAllowGround, fCheckCrime );
				}
			}
			break;

		case OV_TIMERF:
			{
				EXC_SET("TIMERF");
				if ( !strnicmp( s.GetArgStr(), "CLEAR", 5 ) )
				{
					g_World.m_TimedFunctions.Erase(GetUID());
				}
				else if ( !strnicmp( s.GetArgStr(), "STOP", 4 ) )
				{
					g_World.m_TimedFunctions.Stop(GetUID(),s.GetArgStr()+5);
				}
				else
				{
					char *p = s.GetArgRaw();
					int el = Exp_GetVal(p);
					if ( el < 0 )
					{
						g_Log.EventError("TimerF function invalid parameter '%i'.\n", el);
						return false;
					}
					else
					{
						SKIP_ARGSEP(p);
						if ( !*p || ( strlen(p) >= 1024 ))
						{
							g_Log.EventError("TimerF function name empty or args too long - total length must be less than 1024 characters\n");
							return false;
						}
						else
						{
							g_World.m_TimedFunctions.Add(GetUID(), el, p);
						}
					}
				}
			}
			break;
		case OV_TRIGGER:
			{
				if ( s.HasArgs() )
				{
					TRIGRET_TYPE tResult;
					m_CallingObjTrigger = m_RunningTrigger;
					CallPersonalTrigger(s.GetArgRaw(), pSrc, tResult,false);
					m_CallingObjTrigger = NULL;
				}
			} break;
		case OV_DIALOG:
		case OV_SDIALOG:
			{
				EXC_SET("DIALOG or SDIALOG");
				if ( pClientSrc == NULL )
					return false;

				tchar *	Arg_ppCmd[3];		// Maximum parameters in one line
				size_t iQty = Str_ParseCmds( s.GetArgStr(), Arg_ppCmd, CountOf( Arg_ppCmd ));
				if ( iQty < 1 )
					return false;

				if ( index == OV_SDIALOG )
				{
					CResourceID rid = g_Cfg.ResourceGetIDType( RES_DIALOG, Arg_ppCmd[0] );
					int context;

					if ( pClientSrc->GetNetState()->isClientKR() )
					{
						context = g_Cfg.GetKRDialog( (dword)rid ) & 0x00FFFFFF;
					}
					else
					{
						context = ((dword)rid) & 0x00FFFFFF;
					}

					CClient::OpenedGumpsMap_t::iterator itGumpFound = pClientSrc->m_mapOpenedGumps.find( context );

					if ( pCharSrc && (( itGumpFound != pClientSrc->m_mapOpenedGumps.end() ) && ( (*itGumpFound).second > 0 )) )
						break;
				}
				pClientSrc->Dialog_Setup( CLIMODE_DIALOG, g_Cfg.ResourceGetIDType( RES_DIALOG, Arg_ppCmd[0] ),
					iQty > 1 ? Exp_GetVal( Arg_ppCmd[1]) : 0, this, Arg_ppCmd[2] );
			}
			break;
		case OV_DIALOGCLOSE:
			{
				EXC_SET("DIALOGCLOSE");
				if ( pClientSrc == NULL )
					return false;

				tchar *	Arg_ppCmd[2];		// Maximum parameters in one line
				size_t iQty = Str_ParseCmds( s.GetArgStr(), Arg_ppCmd, CountOf( Arg_ppCmd ));
				if ( iQty < 1 )
					return false;

				dword rid = g_Cfg.ResourceGetIDType( RES_DIALOG, Arg_ppCmd[0] );
				if ( pClientSrc->GetNetState()->isClientKR() )
					rid = g_Cfg.GetKRDialog( rid );

				pClientSrc->Dialog_Close( this, rid, iQty > 1 ? Exp_GetVal( Arg_ppCmd[1]) : 0 );
			}
			break;
		case OV_TRYP:
			{
				EXC_SET("TRYP");
				int iMinPriv = s.GetArgVal();

				if ( iMinPriv >= PLEVEL_QTY )
				{
					pSrc->SysMessagef("The %s property can't be changed.", static_cast<lpctstr>(s.GetArgStr()));
					return false;
				}

				if ( pSrc->GetPrivLevel() < iMinPriv )
				{
					pSrc->SysMessagef( "You lack the privilege to change the %s property.", static_cast<lpctstr>(s.GetArgStr()));
					return false;
				}

				// do this verb only if we can touch it.
				if ( pSrc->GetPrivLevel() <= PLEVEL_Counsel )
				{
					if ( pCharSrc == NULL || !pCharSrc->CanTouch(this) )
					{
						pSrc->SysMessagef("Can't touch %s object %s", static_cast<lpctstr>(s.GetArgStr()), GetName());
						return false;
					}
				}
			}
			// no break here, TRYP only does extra checks
		case OV_TRY:
			{
				EXC_SET("TRY or TRYP");
				lpctstr pszVerb = s.GetArgStr();
				CScript script(pszVerb);
				script.m_iResourceFileIndex = s.m_iResourceFileIndex;	// Index in g_Cfg.m_ResourceFiles of the CResourceScript (script file) where the CScript originated
				script.m_iLineNum = s.m_iLineNum;						// Line in the script file where Key/Arg were read
				if ( !r_Verb(script, pSrc) )
				{
					DEBUG_ERR(( "Can't try %s object %s (0%x)\n", pszVerb, GetName(), (dword)(GetUID())));
					return false;
				}
			}
			return true;
		case OV_TRYSRC:
		case OV_TRYSRV:
			{
				EXC_SET("TRYSRC or TRYSRV");
				CUID NewSrc;
				CTextConsole * pNewSrc = NULL;

				if ( index == OV_TRYSRC )
				{
					NewSrc = s.GetArgVal();
					if ( NewSrc.IsValidUID() )
						pNewSrc = NewSrc.CharFind();
				}
				else
					pNewSrc = &g_Serv;

				lpctstr pszVerb = s.GetArgStr();
				if ( pNewSrc == NULL )
				{
					if ( index == OV_TRYSRC )
						DEBUG_ERR(( "Can't trysrc %s object %s (0%x): invalid src uid 0%x\n", pszVerb, GetName(), (dword)(GetUID()), (dword)(NewSrc) ));
					else
						DEBUG_ERR(( "Can't trysrv %s object %s (0%x)\n", pszVerb, GetName(), (dword)(GetUID()) ));

					return false;
				}
				CScript script(pszVerb);
				script.m_iResourceFileIndex = s.m_iResourceFileIndex;	// Index in g_Cfg.m_ResourceFiles of the CResourceScript (script file) where the CScript originated
				script.m_iLineNum = s.m_iLineNum;						// Line in the script file where Key/Arg were read
				if (!r_Verb(script, pNewSrc))
				{
					if ( index == OV_TRYSRC )
						DEBUG_ERR(( "Can't trysrc %s object %s (0%x) with src %s (0%x)\n", pszVerb, GetName(), (dword)(GetUID()), pNewSrc->GetName(), (dword)(NewSrc) ));
					else
						DEBUG_ERR(( "Can't trysrv %s object %s (0%x)\n", pszVerb, GetName(), (dword)(GetUID()) ));

					return false;
				}
			}
			return true;

		case OV_UID:
			EXC_SET("UID");
			// block anyone from ever doing this.
			if ( pSrc )
			{
				pSrc->SysMessage( "Setting the UID this way is not allowed" );
			}
			return false;

		case OV_UPDATE:
			EXC_SET("UPDATE");
			Update();
			break;
		case OV_UPDATEX:
			EXC_SET("UPDATEX");
			// Some things like equipped items need to be removed before they can be updated !
			RemoveFromView();
			Update();
			break;

		case OV_CLICK:
			EXC_SET("CLICK");

			if (!pCharSrc)
				return false;

			if (!pCharSrc->IsClient())
				return false;

			if (s.HasArgs())
			{
				CUID uid = s.GetArgVal();
				if ((!uid.ObjFind()) || (!this->IsChar()))
					return false;
				pCharSrc->GetClient()->Event_SingleClick(uid);
			}
			else
				pCharSrc->GetClient()->Event_SingleClick(this->GetUID());
			return true;

		case OV_DCLICK:
			EXC_SET("DCLICK");
			if (!pCharSrc)
				return false;
			if (s.HasArgs())
			{
				CUID uid = s.GetArgVal();

				if ((!uid.ObjFind()) || (!this->IsChar()))
					return false;

				CChar *pChar = dynamic_cast <CChar *> (this);

				return pChar->Use_Obj(uid.ObjFind(), true, true);
			}
			else
				return pCharSrc->Use_Obj(this, true, true);

		case OV_USEITEM:
			EXC_SET("USEITEM");
			if ( ! pCharSrc )
				return false;
			if ( s.HasArgs() )
			{
				CUID uid = s.GetArgVal();

				if (( ! uid.ObjFind()) || ( ! this->IsChar() ))
					return false;

				CChar *pChar = dynamic_cast <CChar *> (this);

				return pChar->Use_Obj( uid.ObjFind(), false, true );
			}
			else
				return pCharSrc->Use_Obj( this, false, true );

		case OV_FIX:
			s.GetArgStr()[0] = '\0';
		case OV_Z:	//	ussually in "SETZ" form
			EXC_SET("FIX or Z");
			if ( IsItemEquipped())
				return false;
			if ( s.HasArgs())
			{
				SetUnkZ( (char)(s.GetArgVal()));
			}
			else if ( IsTopLevel())
			{
				CChar *pChar = dynamic_cast <CChar *>(this);
				CItem *pItem = dynamic_cast <CItem *>(this);
				if ( pChar )
					SetTopZ(pChar->GetFixZ(GetTopPoint()));
				else if ( pItem )
					SetTopZ(pItem->GetFixZ(GetTopPoint()));
				else
				{
					g_Log.EventDebug("Failed to get reference in FIX or Z\n");
					break;
				}
			}
			Update();
			break;

		default:
			return false;
	}
	return true;
	EXC_CATCH;

	EXC_DEBUG_START;
	EXC_ADD_SCRIPTSRC;
	EXC_DEBUG_END;
	return false;
}

void CObjBase::RemoveFromView( CClient * pClientExclude, bool fHardcoded )
{
	ADDTOCALLSTACK("CObjBase::RemoveFromView");
	// Remove this item from all clients.
	// In a destructor this can do funny things.

	if ( IsDisconnected())
		return;	// not in the world.

	CObjBaseTemplate * pObjTop = GetTopLevelObj();
	CItem * pItem = fHardcoded ? (dynamic_cast<CItem*>(this)) : (NULL);
	CChar * pChar = NULL;


	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if ( pClientExclude == pClient )
			continue;
		pChar = pClient->GetChar();
		if ( pChar == NULL )
			continue;
		if ( pChar->GetTopDistSight( pObjTop ) > pChar->GetVisualRange() )
			//Client does not support removing of items which are farther (will be removed from the radar on the next step, because the server won't resend it)
			continue;
		if ( pItem && pItem->IsItemEquipped() )
		{
			if (( pItem->GetEquipLayer() > LAYER_HORSE ) && ( pItem->GetEquipLayer() != LAYER_BANKBOX ) && ( pItem->GetEquipLayer() != LAYER_DRAGGING ))
				continue;
		}

		if (this->GetEquipLayer() == LAYER_BANKBOX)
			pClient->closeContainer(this);

		pClient->addObjectRemove( this );
	}
}

void CObjBase::ResendOnEquip( bool fAllClients )
{
	ADDTOCALLSTACK("CObjBase::RemoveFromView");
	// Remove this item from all clients if fAllClients == true, from Enhanced Client only if not.
	// Then resend it.

	if ( IsDisconnected())
		return;	// not in the world.

	CObjBaseTemplate * pObjTop = GetTopLevelObj();
	CItem * pItem = dynamic_cast<CItem*>(this);
	CChar * pChar = NULL;


	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		if ( fAllClients == false && !pClient->GetNetState()->isClientEnhanced() )
			continue;
		pChar = pClient->GetChar();
		if ( pChar == NULL )
			continue;
		if ( pChar->GetTopDistSight( pObjTop ) > pChar->GetVisualRange() )
			continue;	//Client does not support removing of items which are farther (will be removed from the radar on the next step, because the server won't resend it)
		if ( pItem )
		{
			if (( pItem->IsItemEquipped() ) && ( !pChar->IsPriv(PRIV_GM) ))
			{
				if (( pItem->GetEquipLayer() > LAYER_HORSE ) && ( pItem->GetEquipLayer() != LAYER_BANKBOX ) && ( pItem->GetEquipLayer() != LAYER_DRAGGING ))
					continue;
			}

			if ( pItem->IsTypeSpellbook() && pItem->IsItemInContainer())
				continue;	// items must be removed from view before equipping in EC when on the floor, however spellbooks cannot be removed from view or client will crash
		}

		if (this->GetEquipLayer() == LAYER_BANKBOX)
			pClient->closeContainer(this);

		pClient->addObjectRemove( this );
		pClient->addItem( pItem );
	}
}

void CObjBase::SetPropertyList(PacketPropertyList* propertyList)
{
	ADDTOCALLSTACK("CObjBase::SetPropertyList");
	// set the property list for this object

	if (propertyList == GetPropertyList())
		return;

	FreePropertyList();
	m_PropertyList = propertyList;
}

void CObjBase::FreePropertyList()
{
	ADDTOCALLSTACK("CObjBase::FreePropertyList");
	// free m_PropertyList

	if (m_PropertyList == nullptr)
		return;

	delete m_PropertyList;
	m_PropertyList = nullptr;
}

dword CObjBase::UpdatePropertyRevision(dword hash)
{
	ADDTOCALLSTACK("CObjBase::UpdatePropertyRevision");

	if (hash != m_PropertyHash)
	{
		// the property list has changed, increment the revision number
		m_PropertyHash = hash;
		++m_PropertyRevision;
	}

	return m_PropertyRevision;
}

void CObjBase::UpdatePropertyFlag(int mask)
{
	ADDTOCALLSTACK("CObjBase::UpdatePropertyFlag");
	if ( g_Serv.IsLoading() || ((g_Cfg.m_iAutoTooltipResend & mask) == 0) )
		return;

	m_fStatusUpdate |= SU_UPDATE_TOOLTIP;

    // Items equipped, inside containers or with timer expired doesn't receive ticks and need to be added to a list of items to be processed separately
    if ( (!IsTopLevel() || IsTimerExpired()) && !g_World.m_ObjStatusUpdates.ContainsPtr(this) )
        g_World.m_ObjStatusUpdates.push_back(this);
}

void CObjBase::OnTickStatusUpdate()
{
	ADDTOCALLSTACK("CObjBase::OnTickStatusUpdate");
	// process m_fStatusUpdate flags

    if (m_fStatusUpdate & SU_UPDATE_TOOLTIP)
    {
        ResendTooltip();
    }

    CCItemDamageable *pItemDmg = static_cast<CCItemDamageable*>(GetComponent(COMP_ITEMDAMAGEABLE));
    if (pItemDmg)
    {
        pItemDmg->OnTickStatsUpdate();
    }
}

void CObjBase::ResendTooltip(bool bSendFull, bool bUseCache)
{
	ADDTOCALLSTACK("CObjBase::ResendTooltip");

	// Send tooltip packet to all nearby clients
	m_fStatusUpdate &= ~SU_UPDATE_TOOLTIP;

	if ( IsAosFlagEnabled(FEATURE_AOS_UPDATE_B) == false )
		return; // tooltips are disabled.
	else if ( IsDisconnected())
		return;	// not in the world.

	if (bUseCache == false)
		FreePropertyList();

	CChar * pChar = NULL;

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != NULL; pClient = it.next())
	{
		pChar = pClient->GetChar();
		if ( pChar == NULL )
			continue;
		if ( !pChar->CanSee( this ) )
			continue;

		pClient->addAOSTooltip(this, bSendFull);
	}
}

void CObjBase::DeletePrepare()
{
	ADDTOCALLSTACK("CObjBase::DeletePrepare");
	// Prepare to delete.
	RemoveFromView();
	RemoveSelf();	// Must remove early or else virtuals will fail.
}

bool CObjBase::IsTriggerActive(lpctstr trig)
{
	return m_RunningTrigger == trig ? true : false;
}

lpctstr CObjBase::GetTriggerActive()
{
	return m_RunningTrigger ? m_RunningTrigger : "none";
}

CVarDefMap * CObjBase::GetTagDefs()
{
	return &m_TagDefs;
}

CCSpawn * CObjBase::GetSpawn()
{
    if (_uidSpawn != UID_UNUSED)
    {
        CItem *pItem = _uidSpawn.ItemFind();
        if (pItem)
        {
            CCSpawn *pSpawn = static_cast<CCSpawn*>(pItem->GetComponent(COMP_SPAWN));
            if (pSpawn)
                return pSpawn;
        }
        _uidSpawn.InitUID();    // for some reason there is an UID assigned but not related to a CItem or CCSpawn, clear it.
    }
    return nullptr;
}

void CObjBase::SetSpawn(CCSpawn * spawn)
{
    if (spawn)
        _uidSpawn = spawn->GetLink()->GetUID();
    else
        _uidSpawn.InitUID();
}

CCFaction * CObjBase::GetFaction()
{
    return static_cast<CCFaction*>(GetComponent(COMP_FACTION));
}

byte CObjBase::RangeL() const
{
	CVarDefCont * pRange = GetDefKey("RANGE", true);
	return (byte)((pRange ? pRange->GetValNum() : 0) & 0xff);
}

byte CObjBase::RangeH() const
{
	CVarDefCont * pRange = GetDefKey("RANGE", true);
	return (byte)(((pRange ? pRange->GetValNum() : 0)>>8) & 0xff);
}

CServerTime CObjBase::GetTimeStamp() const
{
	return m_timestamp;
}

void CObjBase::SetTimeStamp( int64 t_time)
{
	m_timestamp.InitTime(t_time);
}

lpctstr CObjBase::GetDefStr( lpctstr pszKey, bool fZero, bool fDef ) const
{
	CVarDefCont	* pVar = GetDefKey( pszKey, fDef );
	if ( pVar == NULL )
		return (fZero ? "0" : "");
	return pVar->GetValStr();
}

int64 CObjBase::GetDefNum( lpctstr pszKey, bool fZero, bool fDef ) const
{
	CVarDefCont	* pVar = GetDefKey( pszKey, fDef );
	if ( pVar == NULL )
		return (fZero ? 0 : NULL);
	return pVar->GetValNum();
}

void CObjBase::SetTriggerActive(lpctstr trig)
{
	if (trig)
	{
		char *text = Str_GetTemp();
		sprintf(text, "Trigger: %s", trig);
		ADDTOCALLSTACK(text);
	}

	m_RunningTrigger = trig ? trig : NULL;
}

void CObjBase::SetDefNum(lpctstr pszKey, int64 iVal, bool fZero )
{
	m_BaseDefs.SetNum(pszKey, iVal, fZero);
}

void CObjBase::SetDefStr(lpctstr pszKey, lpctstr pszVal, bool fQuoted, bool fZero )
{
	m_BaseDefs.SetStr(pszKey, fQuoted, pszVal, fZero);
}

void CObjBase::DeleteDef(lpctstr pszKey)
{
	m_BaseDefs.DeleteKey(pszKey);
}

CVarDefCont * CObjBase::GetDefKey( lpctstr pszKey, bool fDef ) const
{
	CVarDefCont	* pVar	= m_BaseDefs.GetKey( pszKey );
	if ( !fDef || pVar )
		return pVar;
	if (IsItem())
	{
		CItemBase * pItemDef = static_cast <CItemBase*>( Base_GetDef());
		ASSERT(pItemDef);
		return pItemDef-> m_BaseDefs.GetKey( pszKey );
	}
	else
	{
		CCharBase * pCharDef = static_cast <CCharBase*>( Base_GetDef());
		ASSERT(pCharDef);
		return pCharDef-> m_BaseDefs.GetKey( pszKey );
	}
}

lpctstr CObjBase::GetKeyStr( lpctstr pszKey, bool fZero, bool fDef ) const
{
	CVarDefCont	* pVar = GetKey( pszKey, fDef );
	if ( pVar == NULL )
		return (fZero ? "0" : "");
	return pVar->GetValStr();
}

int64 CObjBase::GetKeyNum( lpctstr pszKey, bool fZero, bool fDef ) const
{
	CVarDefCont	* pVar = GetKey( pszKey, fDef );
	if ( pVar == NULL )
		return (fZero ? 0 : NULL);
	return pVar->GetValNum();
}

CVarDefCont * CObjBase::GetKey( lpctstr pszKey, bool fDef ) const
{
	CVarDefCont	* pVar	= m_TagDefs.GetKey( pszKey );
	if ( !fDef || pVar )
		return pVar;
	if (IsItem())
	{
		CItemBase * pItemDef = static_cast <CItemBase*>( Base_GetDef());
		ASSERT(pItemDef);
		return pItemDef-> m_TagDefs.GetKey( pszKey );
	}
	else
	{
		CCharBase * pCharDef = static_cast <CCharBase*>( Base_GetDef());
		ASSERT(pCharDef);
		return pCharDef-> m_TagDefs.GetKey( pszKey );
	}
}

void CObjBase::SetKeyNum(lpctstr pszKey, int64 iVal)
{
	m_TagDefs.SetNum(pszKey, iVal);
}

void CObjBase::SetKeyStr(lpctstr pszKey, lpctstr pszVal)
{
	m_TagDefs.SetStr(pszKey, false, pszVal);
}

void CObjBase::DeleteKey(lpctstr pszKey)
{
	m_TagDefs.DeleteKey(pszKey);
}

void CObjBase::DupeCopy( const CObjBase * pObj )
{
	CObjBaseTemplate::DupeCopy( pObj );
	m_wHue = pObj->GetHue();
	// m_timeout = pObj->m_timeout;
	m_TagDefs.Copy( &( pObj->m_TagDefs ) );
	m_BaseDefs.Copy(&(pObj->m_BaseDefs));
}

void CObjBase::Delete(bool bforce)
{
	ADDTOCALLSTACK("CObjBase::Delete");
	UNREFERENCED_PARAMETER(bforce);	// CObjBase doesnt use it, but CItem and CChar does use it, do not remove.

    if (GetSpawn())    // If I was created from a Spawn
    {
        //pEntity->Unsuscribe(GetSpawn());    // Avoiding recursive calls from CCSpawn::DelObj when forcing the pChar/pItem to Delete();
        GetSpawn()->DelObj(GetUID());  // Then I should be removed from it's list.
    }
    CEntity *pEntity = static_cast<CEntity*>(this);
    pEntity->Delete(bforce);

	DeletePrepare();
	g_World.m_TimedFunctions.Erase( GetUID() );
	g_World.m_ObjDelete.InsertHead(this);
}

TRIGRET_TYPE CObjBase::Spell_OnTrigger( SPELL_TYPE spell, SPTRIG_TYPE stage, CChar * pSrc, CScriptTriggerArgs * pArgs )
{
	ADDTOCALLSTACK("CObjBase::Spell_OnTrigger");
	CSpellDef	* pSpellDef = g_Cfg.GetSpellDef( spell );
	if ( !pSpellDef )
		return TRIGRET_RET_TRUE;

	if ( pSpellDef->HasTrigger( stage ) )
	{
		// RES_SKILL
		CResourceLock s;
		if ( pSpellDef->ResourceLock( s ))
		{
			return CScriptObj::OnTriggerScript( s, CSpellDef::sm_szTrigName[stage], pSrc, pArgs );
		}
	}
	return TRIGRET_RET_DEFAULT;
}

bool CObjBase::CallPersonalTrigger(tchar * pArgs, CTextConsole * pSrc, TRIGRET_TYPE & trResult, bool bFull)
{
	ADDTOCALLSTACK("CObjBase::CallPersonalTrigger");
	UNREFERENCED_PARAMETER(bFull);
	tchar * ppCmdTrigger[3];
	size_t iResultArgs = Str_ParseCmds(pArgs, ppCmdTrigger, CountOf(ppCmdTrigger), ",");

	if ( iResultArgs > 0 )
	{
		lpctstr callTrigger = ppCmdTrigger[0];
		CScriptTriggerArgs csTriggerArgs;

		if ( iResultArgs == 3 )
		{
			int iTriggerArgType = ATOI(ppCmdTrigger[1]);

			if ( iTriggerArgType == 1 ) // 3 ARGNs
			{
				int64 Arg_piCmd[3];
				iResultArgs = Str_ParseCmds(ppCmdTrigger[2], Arg_piCmd, CountOf(Arg_piCmd), ",");

				if ( iResultArgs == 3 )
					csTriggerArgs.m_iN3 = Arg_piCmd[2];

				if ( iResultArgs >= 2 )
					csTriggerArgs.m_iN2 = Arg_piCmd[1];

				if ( iResultArgs >= 1 )
					csTriggerArgs.m_iN1 = Arg_piCmd[0];
			}
			else if ( iTriggerArgType == 2 ) // ARGS
			{
				csTriggerArgs.m_s1 = ppCmdTrigger[2];
				csTriggerArgs.m_s1_raw = ppCmdTrigger[2];
			}
			else if ( iTriggerArgType == 3 ) // ARGO
			{
				CUID guTriggerArg(Exp_GetVal(ppCmdTrigger[2]));
				CObjBase * pTriggerArgObj = guTriggerArg.ObjFind();
				if ( pTriggerArgObj )
					csTriggerArgs.m_pO1 = pTriggerArgObj;
			}
			else if ( iTriggerArgType == 4 ) // FULL TRIGGER
			{
				tchar * Arg_ppCmd[5];
				iResultArgs = Str_ParseCmds(ppCmdTrigger[2], Arg_ppCmd, CountOf(Arg_ppCmd), ",");

				// ARGS
				if ( iResultArgs == 5 )
				{
					csTriggerArgs.m_s1 = Arg_ppCmd[4];
					csTriggerArgs.m_s1_raw = Arg_ppCmd[4];
				}
				// ARGNs
				if ( iResultArgs >= 4 )
					csTriggerArgs.m_iN3 = Exp_GetVal(Arg_ppCmd[3]);
				if ( iResultArgs >= 3 )
					csTriggerArgs.m_iN2 = Exp_GetVal(Arg_ppCmd[2]);
				if ( iResultArgs >= 2 )
					csTriggerArgs.m_iN1 = Exp_GetVal(Arg_ppCmd[1]);
				// ARGO
				if ( iResultArgs >= 1 )
				{
					CUID guTriggerArg(Exp_GetVal(Arg_ppCmd[0]));
					CObjBase * pTriggerArgObj = guTriggerArg.ObjFind();
					if ( pTriggerArgObj )
						csTriggerArgs.m_pO1 = pTriggerArgObj;
				}
			}
		}

		trResult = OnTrigger(callTrigger, pSrc, &csTriggerArgs);
		return true;
	}

	return false;
}


DIR_TYPE GetDirStr( lpctstr pszDir )
{
	char iDir2, iDir = static_cast< char >( toupper( pszDir[ 0 ] ) );

	switch ( iDir )
	{
		case 'E': return DIR_E;
		case 'W': return DIR_W;
		case 'N':
			iDir2 = static_cast< char >( toupper( pszDir[ 1 ] ) );
			if ( iDir2 == 'E' ) return DIR_NE;
			if ( iDir2 == 'W' ) return DIR_NW;
			return DIR_N;
		case 'S':
			iDir2 = static_cast< char >( toupper( pszDir[ 1 ] ) );
			if ( iDir2 == 'E' ) return DIR_SE;
			if ( iDir2 == 'W' ) return DIR_SW;
			return DIR_S;
		default:
			if ( ( iDir >= '0' ) && ( iDir <= '7' ) )
				return static_cast< DIR_TYPE >( iDir - '0' );
	}
	return DIR_QTY;
}
