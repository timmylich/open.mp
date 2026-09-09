/*
 * Local (main-repo, non-submodule) fixes for open.mp's hardcoded weapon-ID
 * table, layered on top of the vendored SDK the same way
 * Server/Components/Vehicles/vehicle_seats_fix.hpp is layered on top of the
 * vendored vehicle-model table.
 *
 * THE PROBLEM
 * -----------
 * SDK/include/values.hpp defines `MAX_WEAPON_ID = 47`, and
 * SDK/include/player.hpp builds `WeaponInfoList` as a
 * StaticArray<WeaponInfo, MAX_WEAPON_ID> holding only the 47 base-game
 * (SA-MP 0.3.7) weapons, IDs 0-46. Everything that asks "is this a real
 * weapon?" goes through WeaponSlotData, which starts with
 *
 *     if (id >= WeaponInfoList.size()) return INVALID_WEAPON_SLOT / false;
 *
 * A server whose clients run a weapon mod that adds extra weapon IDs (this
 * project uses 100-250 - the gamemode's own item catalogue stores every gun
 * and melee weapon as items.param_1 in that range, and passes it straight to
 * GivePlayerWeapon) therefore fails EVERY one of those checks. The exact same
 * "hardcoded vanilla-only table" bug family as the 400-611 vehicle model
 * range - see vehicle_seats_fix.hpp and the project's
 * .claude/memory/openmp_migration_bugs.md.
 *
 * WHAT THAT ACTUALLY BROKE
 * ------------------------
 * 1. Player-vs-player damage, the headline symptom ("custom weapons deal no
 *    damage, weapon 24 works fine"): the shooter's client sends the
 *    give-damage RPC carrying the REAL weapon ID, and
 *    player_pool.hpp's PlayerGiveTakeDamageRPCHandler::onReceive() dropped
 *    the whole RPC on
 *        `WeaponSlotData(WeaponID).slot() == INVALID_WEAPON_SLOT`
 *    so OnPlayerGiveDamage never fired at all for a custom weapon. Note this
 *    RPC is NOT the bullet-sync packet - the gamemode's
 *    protection/packets.inc rewrites the weapon byte of packet 206 to 24 to
 *    get past check (3) below, but nothing rewrites this RPC, which is why
 *    shots were visible (bullet sync survived) while damage silently vanished.
 * 2. The take-damage RPC, via IsWeaponForTakenDamageValid() (SDK utils.hpp),
 *    which allows an unknown slot only for IDs 49-54 (run-over/drown/explosion
 *    style damage reasons) - custom IDs 100+ fall outside that too.
 * 3. Bullet sync (packet 206) via WeaponSlotData::shootable(), which drives
 *    OnPlayerWeaponShot and lag compensation.
 * 4. Damage to actors, via the same IsWeaponForTakenDamageValid().
 *
 * THE FIX
 * -------
 * Treat an ID in the project's custom range as if it were an ordinary
 * bullet weapon rather than hard-rejecting it. Mirroring the choice already
 * made in vehicle_seats_fix.hpp (fall back to a real, calibrated base-game
 * entry instead of inventing numbers) and the choice the gamemode itself
 * already makes in protection/packets.inc, the stand-in is weapon 24
 * (Desert Eagle) - a plain, unremarkable bullet weapon.
 *
 * Only the damage/shot paths listed above are routed through these wrappers.
 * Everything else that consults WeaponSlotData (ammo bookkeeping in
 * weapons_[], GetWeaponSlot(), NPC weapon handling) is deliberately left
 * alone: those genuinely need a real slot index, and the gamemode drives
 * custom weapons through its own item system rather than through open.mp's
 * per-slot weapon state.
 */
#pragma once

#include <player.hpp>
#include <utils.hpp>
#include <values.hpp>

/// First weapon ID handled by the client-side weapon mod rather than by the
/// base game. Everything below this is either a real GTA weapon (0-46) or one
/// of the special damage reasons (47-54) that open.mp already handles itself,
/// so the fallbacks below must not touch that range.
constexpr int FIRST_CUSTOM_WEAPON_ID = 100;

/// Base-game weapon whose WeaponInfo an unrecognised custom weapon inherits.
/// 24 = Desert Eagle: PlayerWeaponType_Bullet, slot 2.
constexpr uint8_t CUSTOM_WEAPON_FALLBACK_ID = 24;

/// Weapon IDs are transported as uint8_t, so 255 is the hard ceiling.
inline bool isCustomWeaponId(int weapon)
{
	return weapon >= FIRST_CUSTOM_WEAPON_ID && weapon <= 255;
}

/*
 * WeaponSlotData::slot() with the vanilla-only-table restriction relaxed.
 * Returns the real slot for a real weapon, the fallback weapon's slot for a
 * custom one, and INVALID_WEAPON_SLOT for anything genuinely bogus.
 */
inline int8_t getWeaponSlotFixed(int weapon)
{
	if (weapon >= 0 && weapon <= 255)
	{
		int8_t slot = WeaponSlotData(uint8_t(weapon)).slot();
		if (slot != INVALID_WEAPON_SLOT)
		{
			return slot;
		}
	}

	if (isCustomWeaponId(weapon))
	{
		return WeaponSlotData(CUSTOM_WEAPON_FALLBACK_ID).slot();
	}

	return INVALID_WEAPON_SLOT;
}

/*
 * WeaponSlotData::shootable() with the same restriction relaxed. Used only to
 * decide whether an incoming bullet-sync packet is worth parsing; a custom
 * melee weapon being reported as "shootable" here is harmless, because a
 * melee hit never produces a bullet-sync packet in the first place.
 */
inline bool isWeaponShootableFixed(int weapon)
{
	if (weapon >= 0 && weapon <= 255 && WeaponSlotData(uint8_t(weapon)).shootable())
	{
		return true;
	}

	return isCustomWeaponId(weapon);
}

/*
 * IsWeaponForTakenDamageValid() (SDK utils.hpp) with custom IDs accepted.
 * The SDK version already carves out 49-54 as valid damage-but-not-death
 * reasons; this adds the custom range on top and changes nothing else.
 */
inline bool isWeaponForTakenDamageValidFixed(int weapon)
{
	if (IsWeaponForTakenDamageValid(weapon))
	{
		return true;
	}

	return isCustomWeaponId(weapon);
}
