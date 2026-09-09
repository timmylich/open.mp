/*
 * Local (main-repo, non-submodule) fixes layered on top of three vendored SDK
 * functions that all gate on the same Impl::isValidVehicleModel() check
 * (SDK/include/Server/Components/Vehicles/vehicles.hpp:469 - hardcoded
 * `model < 400 || model > 611`), which never recognises this server's custom
 * (27000+) vehicle models even though this fork's own vehicles_impl.hpp::
 * create() deliberately re-enabled spawning them:
 *
 * - Impl::getVehiclePassengerSeats() (vehicle_seats.hpp) returns 0xFF
 *   ("invalid") for any such model.
 * - Impl::getVehicleModelInfo() (vehicle_models.hpp) returns `false` and,
 *   critically, LEAVES ITS OUTPUT PARAMETER COMPLETELY UNTOUCHED for any such
 *   model - see getVehicleModelInfoFixed() below.
 * - Impl::isValidComponentForVehicleModel() (vehicle_components.hpp) returns
 *   `false` for any such model - see isValidComponentForVehicleModelFixed()
 *   below.
 *
 * Deliberately kept OUT of the SDK submodule (see project's
 * .claude/memory/openmp_migration_bugs.md for the full writeup) - these wrap
 * the vendored functions from the main repo instead of patching them in
 * place, so the fix stays visible in a normal `git status`/diff of the main
 * open.mp checkout instead of being hidden inside a submodule.
 */
#pragma once

#include <Server/Components/Vehicles/vehicle_seats.hpp>
#include <Server/Components/Vehicles/vehicle_models.hpp>
#include <Server/Components/Vehicles/vehicle_components.hpp>

/*
 * That 0xFF used to make Vehicle::updateFromPassengerSync() (vehicle.cpp)
 * unconditionally reject every passenger sync packet for a custom-model
 * vehicle: the passenger's position/seat/PlayerState never updated
 * server-side, and OnPlayerUpdate (which the gamemode uses to reset its AFK
 * timer) never fired for them at all - passengers riding a custom vehicle
 * looked permanently AFK, and front/back seat state desynced between players
 * in the same car. It also made NPC::enterVehicle()/putInVehicle() (npc.cpp -
 * what this project's taxi job's NPC_* Pawn calls actually run on) bail out
 * immediately (`passengerSeats == 0xFF` check) without moving, so the taxi
 * NPC never got in the car.
 */
inline uint8_t getVehiclePassengerSeatsFixed(int model)
{
	uint8_t seats = Impl::getVehiclePassengerSeats(model);

	// 0xFF = "unrecognised model" per the SDK's table (see above) - assume
	// the engine's max seat count instead of hard-rejecting. The client only
	// ever sends a SeatID matching the vehicle model it actually loaded.
	return seats == 0xFF ? 8 : seats;
}

/*
 * Impl::getVehicleModelInfo(model, type, out) returns `false` for a custom
 * model WITHOUT writing to `out` at all. Its only caller in this codebase,
 * NPCs/utils.hpp's getVehicleSeatPos(), declares its Vector3 with no
 * initialiser (`Vector3 seatPosFromModelInfo;`) - glm::vec3's default
 * constructor does NOT zero-init without the GLM_FORCE_CTOR_INIT build flag
 * (which this project doesn't set), so a failed lookup silently hands back
 * whatever garbage floats were already sitting on the stack, used directly
 * as a world position for the NPC to walk to.
 *
 * This is the confirmed root cause of the taxi job's NPC "passenger" never
 * getting in custom-model vehicles, and of the intermittent NPC connection
 * drops testers saw seconds after ordering a taxi:
 * - NPC::enterVehicle() (npc.cpp) computes
 *   `distance = glm::distance(getPosition(), getVehicleSeatPos(...))` and
 *   bails out with `if (distance > MAX_DISTANCE_TO_ENTER_VEHICLE) return;`.
 *   Garbage-but-finite coordinates make `distance` huge, so this silently
 *   bails every time (matches "NPC didn't get in, wasn't even there").
 *   Garbage that happens to decode as NaN makes EVERY comparison against it
 *   false per IEEE754 (including this one), so the bail-out guard stops
 *   guarding and the NPC is sent walking toward a NaN target instead -
 *   matches the intermittent instant-to-~1-minute NPC disconnects testers
 *   reported (downstream pathing/streaming code fed a NaN position).
 * - The same garbage-position bug is reachable from
 *   NPC::updateFromSync()/exitVehicle() (npc.cpp:1568) and the vehicle-entry
 *   move-completion handler (npc.cpp:2481) - both also call
 *   getVehicleSeatPos().
 *
 * Fix: fall back to a generic, always-finite seat offset (roughly matching a
 * typical car's real FrontSeat/RearSeat entries in the SDK's own table, see
 * vehicle_models.hpp) instead of leaving `out` uninitialised. Not
 * per-model-accurate for custom vehicles (this project has NO real
 * calibration data source for that on the C++ side - the closest thing,
 * `NYTVehicles` in the gamemode's taxi/util.inc, only feeds the Pawn-side
 * seatid *choice*, not the native NPC_EnterVehicle's own internal walk
 * target), but guarantees a well-defined, near-the-vehicle position instead
 * of undefined behaviour.
 */
inline bool getVehicleModelInfoFixed(int model, VehicleModelInfoType type, Vector3& out)
{
	if (Impl::getVehicleModelInfo(model, type, out))
	{
		return true;
	}

	switch (type)
	{
	case VehicleModelInfo_FrontSeat:
		out = Vector3(0.5f, 0.0f, -0.2f);
		return true;
	case VehicleModelInfo_RearSeat:
		out = Vector3(0.5f, -1.0f, -0.2f);
		return true;
	default:
		out = Vector3(0.0f, 0.0f, 0.0f);
		return true;
	}
}

/*
 * Impl::isValidComponentForVehicleModel(vehicleModel, componentId)
 * (vehicle_components.hpp) has two independent gates: componentId must be in
 * the known component-id range (real crash prevention - see
 * docs/open.mp/scripting/functions/AddVehicleComponent.md: "using an invalid
 * component ID crashes the player's game (fixed in open.mp)", this function
 * IS that fix), AND vehicleModel must be in the base-game 400-611 range (a
 * per-model compatibility bitmask - e.g. not every car has a spoiler slot).
 * For this server's custom (27000+) models the second gate always fails,
 * since there is no compatibility data for them at all - confirmed via
 * Vehicle::addComponent() (vehicle.cpp), which silently no-ops
 * (`if (!isValidComponentForVehicleModel(...)) return;`, no RPC sent, `mods[]`
 * never updated) whenever this returns false. This is the root cause of the
 * tuning saloon's "can't install wheels" report: VehicleTuning_ResetWheels()
 * (gamemode, vehicles/core/functions.inc) calls AddVehicleComponent() with
 * the purchased wheel's component id (the wheel components live at 1025 and
 * 1073-1098, per vehicle_components.hpp's getVehicleComponentSlot() table -
 * matches the range the user guessed), and it's silently dropped for every
 * custom-model vehicle.
 *
 * Fix: re-derive just the componentId bounds check (still reject a genuinely
 * bad id - that's the real crash-prevention half) without the vehicleModel
 * range restriction - same "custom content, don't hard-reject" call as
 * getVehiclePassengerSeatsFixed/getVehicleModelInfoFixed above. A component
 * that's actually incompatible with a *known* (400-611) model is still
 * correctly rejected via the first Impl:: call below.
 */
inline bool isValidComponentForVehicleModelFixed(int vehicleModel, int componentId)
{
	if (Impl::isValidComponentForVehicleModel(vehicleModel, componentId))
	{
		return true;
	}

	int normalizedComponentId = componentId - 1000;
	if (normalizedComponentId < 0 || normalizedComponentId >= MAX_VEHICLE_COMPONENTS - 2)
	{
		// Genuinely out-of-range component id - keep rejecting regardless of
		// model, this is the crash-prevention half of the original check.
		return false;
	}

	// componentId is real; only the 400-611 vehicleModel gate rejected it.
	return vehicleModel < 400 || vehicleModel > 611;
}
