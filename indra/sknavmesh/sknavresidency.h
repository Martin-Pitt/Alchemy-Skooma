/**
 * @file sknavresidency.h
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

#ifndef SK_NAVRESIDENCY_H
#define SK_NAVRESIDENCY_H

#include "sknavenclosure.h"
#include "sknavspaces.h"

#include "lluuid.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sknav
{
    /// The products a mesh settles into. Its detail air may leave memory only
    /// once none of them is pending for it; a new product joins by adding itself here.
    class SettleGate
    {
    public:
        typedef std::function<bool(const LLUUID& mesh)> PendingFn;

        /// reads_air: whether the product, once pending again, needs the detail back
        /// (spaces do; probe placement reads the spaces and only waits its turn).
        void add(const std::string& name, PendingFn pending, bool reads_air = true);
        /// The first product still pending for the mesh, or null when none is.
        const char* pending(const LLUUID& mesh) const;
        bool settled(const LLUUID& mesh) const { return pending(mesh) == nullptr; }
        /// Whether a product that reads the air is pending: a spilled mesh's detail comes back for it.
        bool needsDetail(const LLUUID& mesh) const;
        size_t size() const { return mProducts.size(); }

    private:
        struct Product
        {
            std::string mName;
            PendingFn mPending;
            bool mReadsAir = true;
        };
        std::vector<Product> mProducts;
    };

    /// Per tile key, the air a product should point at instead; null lets the detail go.
    typedef std::map<TileKey, TileAirPtr> AirSwap;

    /// Copies whose listed tiles point at other air and are otherwise the same:
    /// a landed product is replaced, never rewritten, as a worker may be reading it.
    EnclosureProductPtr withTileAir(const EnclosureProduct& product, const AirSwap& swap);
    RoomsProductPtr withTileAir(const RoomsProduct& product, const AirSwap& swap);
}

#endif // SK_NAVRESIDENCY_H
