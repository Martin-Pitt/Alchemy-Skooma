/**
 * @file sknavresidency.cpp
 * @brief What keeps a mesh's detail air in memory, and the products re-pointed at it as it leaves and returns.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "sknavresidency.h"

namespace sknav
{
    void SettleGate::add(const std::string& name, PendingFn pending)
    {
        mProducts.emplace_back(name, std::move(pending));
    }

    const char* SettleGate::pending(const LLUUID& mesh) const
    {
        for (const auto& product : mProducts)
        {
            if (product.second && product.second(mesh))
            {
                return product.first.c_str();
            }
        }
        return nullptr;
    }

    EnclosureProductPtr withTileAir(const EnclosureProduct& product, const AirSwap& swap)
    {
        auto out = std::make_shared<EnclosureProduct>(product);
        for (TileEnclosure& t : out->mTiles)
        {
            if (const auto it = swap.find(t.mKey); it != swap.end())
            {
                t.mAir = it->second;
            }
        }
        return out;
    }

    RoomsProductPtr withTileAir(const RoomsProduct& product, const AirSwap& swap)
    {
        auto out = std::make_shared<RoomsProduct>(product);
        for (RoomsTile& t : out->mTiles)
        {
            if (const auto it = swap.find(t.mKey); it != swap.end())
            {
                t.mAir = it->second;
            }
        }
        return out;
    }
}
