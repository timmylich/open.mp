/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2022, open.mp team and contributors.
 */

#include "textdraw.hpp"
#include <Impl/pool_impl.hpp>
#include <netcode.hpp>

using namespace Impl;

// Local (main-repo, non-submodule) override of the SDK's
// PLAYER_TEXTDRAW_POOL_SIZE (SDK/include/values.hpp, still 256 there - the
// vendored constant is left untouched on purpose, see
// .claude/memory/openmp_migration_bugs.md). PLAYER_TEXTDRAW_POOL_SIZE is
// only ever consumed here (the storage pool's size template argument, one
// line below), so redefining it under a local name and using that instead is
// enough to raise the limit without editing the submodule.
//
// NOTE: 256 is documented as the classic SA-MP client's hard, compiled-in
// per-player textdraw limit (docs/open.mp/scripting/resources/limits.md,
// "Created Serverwise (Per-Player)"). Raising the SERVER's pool only helps if
// the client build actually in use also supports more than 256 - confirm
// that on the live client before relying on ids >= 256 actually rendering.
// The concrete bug that originally motivated trying this (open.mp inventory/
// admin-spectate textdraws not rendering) turned out to be unrelated - see
// openmp_migration_bugs.md - so this raise may not be load-bearing any more.
constexpr int LOCAL_PLAYER_TEXTDRAW_POOL_SIZE = 1024;

class PlayerTextDrawData final : public IPlayerTextDrawData
{
private:
	IPlayer& player;
	MarkedPoolStorage<PlayerTextDraw, IPlayerTextDraw, 0, LOCAL_PLAYER_TEXTDRAW_POOL_SIZE> storage;
	bool selecting;

public:
	inline void cancelSelecting()
	{
		selecting = false;
	}

	PlayerTextDrawData(IPlayer& player)
		: player(player)
		, selecting(false)
	{
	}

	void beginSelection(Colour highlight) override
	{
		selecting = true;
		NetCode::RPC::PlayerBeginTextDrawSelect beginTextDrawSelectRPC;
		beginTextDrawSelectRPC.Enable = true;
		beginTextDrawSelectRPC.Col = highlight;
		PacketHelper::send(beginTextDrawSelectRPC, player);
	}

	bool isSelecting() const override
	{
		return selecting;
	}

	void endSelection() override
	{
		selecting = false;
		NetCode::RPC::PlayerBeginTextDrawSelect beginTextDrawSelectRPC;
		beginTextDrawSelectRPC.Enable = false;
		beginTextDrawSelectRPC.Col = Colour::None();
		PacketHelper::send(beginTextDrawSelectRPC, player);
	}

	IPlayerTextDraw* create(Vector2 position, StringView text) override
	{
		return storage.emplace(player, position, text);
	}

	IPlayerTextDraw* create(Vector2 position, int model) override
	{
		return storage.emplace(player, position, "_", TextDrawStyle_Preview, model);
	}

	void freeExtension() override
	{
		delete this;
	}

	void reset() override
	{
		selecting = false;
		storage.clear();
	}

	virtual Pair<size_t, size_t> bounds() const override
	{
		return std::make_pair(storage.Lower, storage.Upper);
	}

	IPlayerTextDraw* get(int index) override
	{
		return storage.get(index);
	}

	void release(int index) override
	{
		PlayerTextDraw* td = storage.get(index);
		if (td)
		{
			td->destream();
			storage.release(index, false);
		}
	}

	void lock(int index) override
	{
		storage.lock(index);
	}

	bool unlock(int index) override
	{
		return storage.unlock(index);
	}

	IEventDispatcher<PoolEventHandler<IPlayerTextDraw>>& getPoolEventDispatcher() override
	{
		return storage.getEventDispatcher();
	}

	/// Get a set of all the available labels
	const FlatPtrHashSet<IPlayerTextDraw>& entries() override
	{
		return storage._entries();
	}
};

class TextDrawsComponent final : public ITextDrawsComponent, public PlayerConnectEventHandler, public PoolEventHandler<IPlayer>
{
private:
	ICore* core = nullptr;
	MarkedPoolStorage<TextDraw, ITextDraw, 0, GLOBAL_TEXTDRAW_POOL_SIZE> storage;
	DefaultEventDispatcher<TextDrawEventHandler> dispatcher;

public:
	StringView componentName() const override
	{
		return "TextDraws";
	}

	SemanticVersion componentVersion() const override
	{
		return SemanticVersion(OMP_VERSION_MAJOR, OMP_VERSION_MINOR, OMP_VERSION_PATCH, BUILD_NUMBER);
	}

	struct PlayerSelectTextDrawEventHandler : public SingleNetworkInEventHandler
	{
		TextDrawsComponent& self;
		PlayerSelectTextDrawEventHandler(TextDrawsComponent& self)
			: self(self)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			NetCode::RPC::OnPlayerSelectTextDraw RPC;
			if (!RPC.read(bs))
			{
				return false;
			}

			PlayerTextDrawData* data = queryExtension<PlayerTextDrawData>(peer);
			if (data)
			{
				if (RPC.Invalid)
				{
					data->cancelSelecting();
					self.dispatcher.all(
						[&peer](TextDrawEventHandler* handler)
						{
							handler->onPlayerCancelTextDrawSelection(peer);
						});
				}
				else
				{
					if (RPC.PlayerTextDraw)
					{
						ScopedPoolReleaseLock lock(*data, RPC.TextDrawID);
						if (lock.entry && lock.entry->isSelectable() && lock.entry->isShown())
						{
							self.dispatcher.dispatch(&TextDrawEventHandler::onPlayerClickPlayerTextDraw, peer, *lock.entry);
						}
					}
					else if (!RPC.PlayerTextDraw)
					{
						ScopedPoolReleaseLock lock(self, RPC.TextDrawID);
						if (lock.entry && lock.entry->isSelectable() && lock.entry->isShownForPlayer(peer))
						{
							self.dispatcher.dispatch(&TextDrawEventHandler::onPlayerClickTextDraw, peer, *lock.entry);
						}
					}
				}
			}

			return true;
		}
	} playerSelectTextDrawEventHandler;

	TextDrawsComponent()
		: playerSelectTextDrawEventHandler(*this)
	{
	}

	void onLoad(ICore* c) override
	{
		core = c;
		core->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
		core->getPlayers().getPoolEventDispatcher().addEventHandler(this);
		NetCode::RPC::OnPlayerSelectTextDraw::addEventHandler(*core, &playerSelectTextDrawEventHandler);
	}

	void reset() override
	{
		// Destroy all stored entity instances.
		storage.clear();
	}

	~TextDrawsComponent()
	{
		if (core)
		{
			core->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
			core->getPlayers().getPoolEventDispatcher().removeEventHandler(this);
			NetCode::RPC::OnPlayerSelectTextDraw::removeEventHandler(*core, &playerSelectTextDrawEventHandler);
		}
	}

	void onPlayerConnect(IPlayer& player) override
	{
		player.addExtension(new PlayerTextDrawData(player), true);
	}

	void onPoolEntryDestroyed(IPlayer& player) override
	{
		const int pid = player.getID();

		for (ITextDraw* textdraw : storage)
		{
			TextDraw* textdraw_ = static_cast<TextDraw*>(textdraw);
			textdraw_->removeFor(pid, player);
		}
	}

	IEventDispatcher<TextDrawEventHandler>& getEventDispatcher() override
	{
		return dispatcher;
	}

	ITextDraw* create(Vector2 position, StringView text) override
	{
		return storage.emplace(position, text);
	}

	ITextDraw* create(Vector2 position, int model) override
	{
		return storage.emplace(position, "_", TextDrawStyle_Preview, model);
	}

	void free() override
	{
		delete this;
	}

	virtual Pair<size_t, size_t> bounds() const override
	{
		return std::make_pair(storage.Lower, storage.Upper);
	}

	ITextDraw* get(int index) override
	{
		return storage.get(index);
	}

	void release(int index) override
	{
		auto ptr = storage.get(index);
		if (ptr)
		{
			static_cast<TextDraw*>(ptr)->destream();
			storage.release(index, false);
		}
	}

	void lock(int index) override
	{
		storage.lock(index);
	}

	bool unlock(int index) override
	{
		return storage.unlock(index);
	}

	const FlatPtrHashSet<ITextDraw>& entries() override
	{
		return storage._entries();
	}

	IEventDispatcher<PoolEventHandler<ITextDraw>>& getPoolEventDispatcher() override
	{
		return storage.getEventDispatcher();
	}
};

COMPONENT_ENTRY_POINT()
{
	return new TextDrawsComponent();
}
