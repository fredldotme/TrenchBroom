/*
 Copyright (C) 2010-2012 Kristian Duske
 
 This file is part of TrenchBroom.
 
 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with TrenchBroom.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __TrenchBroom__MoveObjectsCommand__
#define __TrenchBroom__MoveObjectsCommand__

#include "Controller/Command.h"
#include "Utility/VecMath.h"

using namespace TrenchBroom::Math;

namespace TrenchBroom {
    namespace Model {
        class MapDocument;
    }
    
    namespace Controller {
        class MoveObjectsCommand : public DocumentCommand {
        private:
            Vec3f m_delta;
            bool m_lockTextures;

            bool translate(const Vec3f& delta);
            
            bool performDo();
            bool performUndo();
            
            MoveObjectsCommand(Model::MapDocument& document, const wxString& name, const Vec3f& delta, bool lockTextures);
        public:
            static MoveObjectsCommand* moveObjects(Model::MapDocument& document, const wxString& name, const Vec3f& delta, bool lockTextures);
        };
    }
}

#endif /* defined(__TrenchBroom__MoveObjectsCommand__) */
