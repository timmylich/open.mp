/*
 * Local (main-repo, non-submodule) fix layered on top of the vendored SDK's
 * Impl::getVehiclePassengerSeats() (SDK/include/Server/Components/Vehicles/
 * vehicle_seats.hpp). The SDK's internal seat table only recognises the base
 * game's 400-611 vehicle model range and returns 0xFF ("invalid") for
 * anything outside it - including this server's custom (27000+) vehicle
 * models, re-enabled by this fork's own vehicles_impl.hpp::create() no
 * longer rejecting them.
 *
 * That 0xFF used to make Vehicle::updateFromPassengerSync() (vehicle.cpp)
 * unconditionally reject every passenger sync packet for a custom-model
 * vehicle: the passenger's position/seat/PlayerState never updated
 * server-side, and OnPlayerUpdate (which the gamemode uses to reset its AFK
 * timer) never fired for them at all - passengers riding a custom vehicle
 * looked permanently AFK, and front/back seat state desynced between players
 * in the same car. It also made NPC::enterVehicle()/putInVehicle() (npc.cpp -
 * what this project's taxi job's NPC_* Pawn calls actually run on) return
 * immediately without moving, so the taxi NPC never got in the car.
 *
 * Deliberately kept OUT of the SDK submodule (see project's
 * .claude/memory/openmp_migration_bugs.md for the full writeup) - this wraps
 * the vendored function from the main repo instead of patching it in place,
 * so the fix stays visible in a normal `git status`/diff of the main open.mp
 * checkout instead of being hidden inside a submodule.
 */
#pragma once

#include <Server/Components/Vehicles/vehicle_seats.hpp>

inline uint8_t getVehiclePassengerSeatsFixed(int model)
{
	uint8_t seats = Impl::getVehiclePassengerSeats(model);

	// 0xFF = "unrecognised model" per the SDK's table (see above) - assume
	// the engine's max seat count instead of hard-rejecting. The client only
	// ever sends a SeatID matching the vehicle model it actually loaded.
	return seats == 0xFF ? 8 : seats;
}
