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
 along with TrenchBroom.  If not, see <http://www.gnu.org/licenses/>.C
 */

#include "MapRenderer.h"

#include "IO/FileManager.h"
#include "Model/Brush.h"
#include "Model/BrushGeometry.h"
#include "Model/EditStateManager.h"
#include "Model/Entity.h"
#include "Model/EntityDefinition.h"
#include "Model/Filter.h"
#include "Model/Map.h"
#include "Model/MapDocument.h"
#include "Renderer/EdgeRenderer.h"
#include "Renderer/EntityRenderer.h"
#include "Renderer/FaceRenderer.h"
#include "Renderer/Figure.h"
#include "Renderer/RenderContext.h"
#include "Renderer/SharedResources.h"
#include "Renderer/TextureRenderer.h"
#include "Renderer/TextureRendererManager.h"
#include "Renderer/Shader/Shader.h"
#include "Renderer/Shader/ShaderManager.h"
#include "Renderer/Shader/ShaderProgram.h"
#include "Renderer/Vbo.h"
#include "Renderer/Text/FontDescriptor.h"
#include "Renderer/Text/StringManager.h"
#include "Utility/Grid.h"
#include "Utility/Preferences.h"

namespace TrenchBroom {
    namespace Renderer {
        static const int IndexSize = sizeof(GLuint);
        static const int VertexSize = 3 * sizeof(GLfloat);
        static const int ColorSize = 4;
        static const int TexCoordSize = 2 * sizeof(GLfloat);
        static const int FaceVertexSize = TexCoordSize + TexCoordSize + VertexSize;
        static const int EdgeVertexSize = ColorSize + VertexSize;
        static const int EntityBoundsVertexSize = ColorSize + VertexSize;

        void MapRenderer::rebuildGeometryData(RenderContext& context) {
            if (!m_geometryDataValid) {
                m_faceRenderer = FaceRendererPtr(NULL);
                m_edgeRenderer = EdgeRendererPtr(NULL);
            }
            if (!m_selectedGeometryDataValid) {
                m_selectedFaceRenderer = FaceRendererPtr(NULL);
                m_selectedEdgeRenderer = EdgeRendererPtr(NULL);
            }
            if (!m_lockedGeometryDataValid) {
                m_lockedFaceRenderer = FaceRendererPtr(NULL);
                m_lockedEdgeRenderer = EdgeRendererPtr(NULL);
            }
            
            FaceSorter unselectedFaceSorter;
            FaceSorter selectedFaceSorter;
            FaceSorter lockedFaceSorter;
            
            Model::BrushList unselectedWorldBrushes;
            Model::BrushList unselectedEntityBrushes;
            Model::BrushList selectedBrushes;
            Model::BrushList lockedBrushes;
            Model::FaceList partiallySelectedBrushFaces;
            unsigned int totalUnselectedEdgeVertexCount = 0;
            unsigned int totalSelectedEdgeVertexCount = 0;
            unsigned int totalLockedEdgeVertexCount = 0;
            
            // collect all visible faces and brushes
            const Model::EntityList& entities = m_document.map().entities();
            for (unsigned int i = 0; i < entities.size(); i++) {
                Model::Entity* entity = entities[i];
                const Model::BrushList& brushes = entity->brushes();
                for (unsigned int j = 0; j < brushes.size(); j++) {
                    Model::Brush* brush = brushes[j];
                    if (context.filter().brushVisible(*brush)) {
                        if (entity->selected() || brush->selected()) {
                            selectedBrushes.push_back(brush);
                            totalSelectedEdgeVertexCount += (2 * brush->edges().size());
                        } else if (entity->locked() || brush->locked()) {
                            lockedBrushes.push_back(brush);
                            totalLockedEdgeVertexCount+= (2 * brush->edges().size());
                        } else {
                            if (entity->worldspawn())
                                unselectedWorldBrushes.push_back(brush);
                            else
                                unselectedEntityBrushes.push_back(brush);
                            totalUnselectedEdgeVertexCount += (2 * brush->edges().size());
                            if (brush->partiallySelected()) {
                                const Model::FaceList& faces = brush->faces();
                                for (unsigned int k = 0; k < faces.size(); k++) {
                                    Model::Face* face = faces[k];
                                    if (face->selected()) {
                                        partiallySelectedBrushFaces.push_back(face);
                                        totalSelectedEdgeVertexCount += (2 * face->edges().size());
                                    }
                                }
                            }
                        }
                        
                        const Model::FaceList& faces = brush->faces();
                        for (unsigned int k = 0; k < faces.size(); k++) {
                            Model::Face* face = faces[k];
                            Model::Texture* texture = face->texture();
                            if (entity->selected() || brush->selected() || face->selected())
                                selectedFaceSorter.addPolygon(texture, face, face->vertices().size());
                            else if (entity->locked() || brush->locked())
                                lockedFaceSorter.addPolygon(texture, face, face->vertices().size());
                            else
                                unselectedFaceSorter.addPolygon(texture, face, face->vertices().size());
                        }
                    }
                }
            }
            
            // merge the collected brushes
            Model::BrushList unselectedBrushes(unselectedWorldBrushes);
            unselectedBrushes.insert(unselectedBrushes.end(), unselectedEntityBrushes.begin(), unselectedEntityBrushes.end());

            // write face triangles
            m_faceVbo->activate();
            m_faceVbo->map();
            
            // make sure that the VBO is sufficiently large
            size_t totalFaceVertexCount = unselectedFaceSorter.vertexCount() + selectedFaceSorter.vertexCount() + lockedFaceSorter.vertexCount();
            size_t totalPolygonCount = unselectedFaceSorter.polygonCount() + selectedFaceSorter.polygonCount() + lockedFaceSorter.polygonCount();
            size_t totalTriangleVertexCount = 3 * totalFaceVertexCount - 2 * totalPolygonCount;
            m_faceVbo->ensureFreeCapacity(static_cast<unsigned int>(totalTriangleVertexCount) * FaceVertexSize);
            
            Preferences::PreferenceManager& prefs = Preferences::PreferenceManager::preferences();
            TextureRendererManager& textureRendererManager = m_document.sharedResources().textureRendererManager();
            const Color& faceColor = prefs.getColor(Preferences::FaceColor);

            if (!m_geometryDataValid && !unselectedFaceSorter.empty())
                m_faceRenderer = FaceRendererPtr(new FaceRenderer(*m_faceVbo, textureRendererManager, unselectedFaceSorter, faceColor));
            if (!m_selectedGeometryDataValid && !selectedFaceSorter.empty())
                m_selectedFaceRenderer = FaceRendererPtr(new FaceRenderer(*m_faceVbo, textureRendererManager, selectedFaceSorter, faceColor));
            if (!m_lockedGeometryDataValid && !lockedFaceSorter.empty())
                m_lockedFaceRenderer = FaceRendererPtr(new FaceRenderer(*m_faceVbo, textureRendererManager, lockedFaceSorter, faceColor));
            
            m_faceVbo->unmap();
            m_faceVbo->deactivate();
            
            // write edges
            m_edgeVbo->activate();
            m_edgeVbo->map();
            m_edgeVbo->ensureFreeCapacity(totalUnselectedEdgeVertexCount * EdgeVertexSize + (totalSelectedEdgeVertexCount + totalLockedEdgeVertexCount) * VertexSize);
            
            const Color& edgeColor = prefs.getColor(Preferences::EdgeColor);

            if (!m_geometryDataValid && !unselectedBrushes.empty())
                m_edgeRenderer = EdgeRendererPtr(new EdgeRenderer(*m_edgeVbo, unselectedBrushes, Model::EmptyFaceList, edgeColor));
            if (!m_selectedGeometryDataValid && (!selectedBrushes.empty() || !partiallySelectedBrushFaces.empty()))
                m_selectedEdgeRenderer = EdgeRendererPtr(new EdgeRenderer(*m_edgeVbo, selectedBrushes, partiallySelectedBrushFaces));
            if (!m_lockedGeometryDataValid && !lockedBrushes.empty())
                m_lockedEdgeRenderer = EdgeRendererPtr(new EdgeRenderer(*m_edgeVbo, lockedBrushes, Model::EmptyFaceList));
            
            m_edgeVbo->unmap();
            m_edgeVbo->deactivate();
            
            m_geometryDataValid = true;
            m_selectedGeometryDataValid = true;
            m_lockedGeometryDataValid = true;
        }
        
        void MapRenderer::deleteFigures(FigureList& figures) {
            FigureList::iterator it, end;
            for (it = figures.begin(), end = figures.end(); it != end; ++it) {
                Figure* figure = *it;
                delete figure;
            }
            figures.clear();
        }

        void MapRenderer::validate(RenderContext& context) {
            if (!m_geometryDataValid || !m_selectedGeometryDataValid || !m_lockedGeometryDataValid)
                rebuildGeometryData(context);
        }
        
        void MapRenderer::renderFaces(RenderContext& context) {
            Preferences::PreferenceManager& prefs = Preferences::PreferenceManager::preferences();
            
            m_faceVbo->activate();
            if (m_faceRenderer.get() != NULL)
                m_faceRenderer->render(context, false);
            if (context.viewOptions().renderSelection() && m_selectedFaceRenderer.get() != NULL)
                m_selectedFaceRenderer->render(context, false, prefs.getColor(Preferences::SelectedFaceColor));
            if (m_lockedFaceRenderer.get() != NULL)
                m_lockedFaceRenderer->render(context, true, prefs.getColor(Preferences::LockedFaceColor));
            m_faceVbo->deactivate();
        }
        
        void MapRenderer::renderEdges(RenderContext& context) {
            Preferences::PreferenceManager& prefs = Preferences::PreferenceManager::preferences();
            
            m_edgeVbo->activate();
            if (m_edgeRenderer.get() != NULL) {
                glSetEdgeOffset(0.02f);
                m_edgeRenderer->render(context);
            }
            if (m_lockedEdgeRenderer.get() != NULL) {
                glSetEdgeOffset(0.02f);
                m_edgeRenderer->render(context, prefs.getColor(Preferences::LockedEdgeColor));
            }
            if (context.viewOptions().renderSelection() && m_selectedEdgeRenderer.get() != NULL) {
                glDisable(GL_DEPTH_TEST);
                glSetEdgeOffset(0.02f);
                m_selectedEdgeRenderer->render(context, prefs.getColor(Preferences::OccludedSelectedEdgeColor));
                glEnable(GL_DEPTH_TEST);
                glSetEdgeOffset(0.025f);
                m_selectedEdgeRenderer->render(context, prefs.getColor(Preferences::SelectedEdgeColor));
            }
            m_edgeVbo->deactivate();
            glResetEdgeOffset();
        }
        
        void MapRenderer::renderFigures(RenderContext& context) {
            FigureList::iterator it, end;
            for (it = m_figures.begin(), end = m_figures.end(); it != end; ++it) {
                Figure* figure = *it;
                figure->render(*m_figureVbo, context);
            }
        }

        MapRenderer::MapRenderer(Model::MapDocument& document) :
        m_document(document) {
            m_rendering = false;

            m_faceVbo = VboPtr(new Vbo(GL_ARRAY_BUFFER, 0xFFFF));
            m_edgeVbo = VboPtr(new Vbo(GL_ARRAY_BUFFER, 0xFFFF));
            m_entityVbo = VboPtr(new Vbo(GL_ARRAY_BUFFER, 0xFFFF));
            m_figureVbo = VboPtr(new Vbo(GL_ARRAY_BUFFER, 0xFFFF));
            
            m_geometryDataValid = false;
            m_selectedGeometryDataValid = false;
            m_lockedGeometryDataValid = false;

            Preferences::PreferenceManager& prefs = Preferences::PreferenceManager::preferences();
            
            m_entityRenderer = EntityRendererPtr(new EntityRenderer(*m_entityVbo, m_document));
            m_entityRenderer->setClassnameFadeDistance(prefs.getFloat(Preferences::InfoOverlayFadeDistance));
            m_entityRenderer->setClassnameColor(prefs.getColor(Preferences::InfoOverlayTextColor), prefs.getColor(Preferences::InfoOverlayBackgroundColor));
            
            m_selectedEntityRenderer = EntityRendererPtr(new EntityRenderer(*m_entityVbo, m_document));
            m_selectedEntityRenderer->setClassnameFadeDistance(prefs.getFloat(Preferences::SelectedInfoOverlayFadeDistance));
            m_selectedEntityRenderer->setClassnameColor(prefs.getColor(Preferences::SelectedInfoOverlayTextColor), prefs.getColor(Preferences::SelectedInfoOverlayBackgroundColor));
            m_selectedEntityRenderer->setOccludedClassnameColor(prefs.getColor(Preferences::SelectedInfoOverlayTextColor), prefs.getColor(Preferences::SelectedInfoOverlayBackgroundColor));
            m_selectedEntityRenderer->setBoundsColor(prefs.getColor(Preferences::SelectedEntityBoundsColor));
            m_selectedEntityRenderer->setOccludedBoundsColor(prefs.getColor(Preferences::OccludedSelectedEntityBoundsColor));
            m_selectedEntityRenderer->setTintColor(prefs.getColor(Preferences::SelectedEntityColor));
            
            m_lockedEntityRenderer = EntityRendererPtr(new EntityRenderer(*m_entityVbo, m_document));
            m_lockedEntityRenderer->setClassnameFadeDistance(prefs.getFloat(Preferences::InfoOverlayFadeDistance));
            m_lockedEntityRenderer->setClassnameColor(prefs.getColor(Preferences::LockedInfoOverlayTextColor), prefs.getColor(Preferences::LockedInfoOverlayBackgroundColor));
            m_lockedEntityRenderer->setBoundsColor(prefs.getColor(Preferences::LockedEntityBoundsColor));
            m_lockedEntityRenderer->setTintColor(prefs.getColor(Preferences::LockedEntityColor));
            m_lockedEntityRenderer->setGrayscale(true);
        }
        
        MapRenderer::~MapRenderer() {
            deleteFigures(m_deletedFigures);
            deleteFigures(m_figures);
        }

        void MapRenderer::addEntity(Model::Entity& entity) {
            m_entityRenderer->addEntity(entity);
        }

        void MapRenderer::addEntities(const Model::EntityList& entities) {
            m_entityRenderer->addEntities(entities);
        }

        void MapRenderer::removeEntity(Model::Entity& entity) {
            m_entityRenderer->removeEntity(entity);
        }
        
        void MapRenderer::removeEntities(const Model::EntityList& entities) {
            m_entityRenderer->removeEntities(entities);
        }
        
        void MapRenderer::changeEditState(const Model::EditStateChangeSet& changeSet) {
            m_entityRenderer->addEntities(changeSet.entitiesTo(Model::EditState::Default));
            m_entityRenderer->removeEntities(changeSet.entitiesFrom(Model::EditState::Default));
            m_selectedEntityRenderer->addEntities(changeSet.entitiesTo(Model::EditState::Selected));
            m_selectedEntityRenderer->removeEntities(changeSet.entitiesFrom(Model::EditState::Selected));
            m_lockedEntityRenderer->addEntities(changeSet.entitiesTo(Model::EditState::Locked));
            m_lockedEntityRenderer->removeEntities(changeSet.entitiesFrom(Model::EditState::Locked));
            
            if (changeSet.brushStateChangedFrom(Model::EditState::Default) ||
                changeSet.brushStateChangedTo(Model::EditState::Default) ||
                changeSet.faceSelectionChanged()) {
                m_geometryDataValid = false;
            }

            if (changeSet.brushStateChangedFrom(Model::EditState::Selected) ||
                changeSet.brushStateChangedTo(Model::EditState::Selected) ||
                changeSet.faceSelectionChanged()) {
                m_selectedGeometryDataValid = false;

                const Model::BrushList& selectedBrushes = changeSet.brushesTo(Model::EditState::Selected);
                for (unsigned int i = 0; i < selectedBrushes.size(); i++) {
                    Model::Brush* brush = selectedBrushes[i];
                    Model::Entity* entity = brush->entity();
                    if (!entity->worldspawn() && entity->partiallySelected()) {
                        m_entityRenderer->removeEntity(*entity);
                        m_selectedEntityRenderer->addEntity(*entity);
                    }
                }

                const Model::BrushList& deselectedBrushes = changeSet.brushesFrom(Model::EditState::Selected);
                for (unsigned int i = 0; i < deselectedBrushes.size(); i++) {
                    Model::Brush* brush = deselectedBrushes[i];
                    Model::Entity* entity = brush->entity();
                    if (!entity->worldspawn() && !entity->partiallySelected()) {
                        m_selectedEntityRenderer->removeEntity(*entity);
                        m_entityRenderer->addEntity(*entity);
                    }
                }
            }
            
            if (changeSet.brushStateChangedFrom(Model::EditState::Locked) ||
                changeSet.brushStateChangedTo(Model::EditState::Locked) ||
                changeSet.faceSelectionChanged()) {
                m_lockedGeometryDataValid = false;
            }
        }

        void MapRenderer::loadMap() {
            clearMap();
            addEntities(m_document.map().entities());
        }
        
        void MapRenderer::clearMap() {
            m_faceRenderer = FaceRendererPtr(NULL);
            m_selectedFaceRenderer = FaceRendererPtr(NULL);
            m_lockedFaceRenderer = FaceRendererPtr(NULL);
            m_edgeRenderer = EdgeRendererPtr(NULL);
            m_selectedEdgeRenderer = EdgeRendererPtr(NULL);
            m_lockedEdgeRenderer = EdgeRendererPtr(NULL);
            m_entityRenderer->clear();
            m_selectedEntityRenderer->clear();
            m_lockedEntityRenderer->clear();

            invalidateAll();
            invalidateEntityModelRendererCache();
        }

        void MapRenderer::invalidateEntities() {
            m_entityRenderer->invalidateBounds();
            m_selectedEntityRenderer->invalidateBounds();
            m_lockedEntityRenderer->invalidateBounds();
        }
        
        void MapRenderer::invalidateSelectedEntities() {
            m_selectedEntityRenderer->invalidateBounds();
        }

        void MapRenderer::invalidateBrushes() {
            m_geometryDataValid = false;
            m_selectedGeometryDataValid = false;
            m_lockedGeometryDataValid = false;
        }

        void MapRenderer::invalidateSelectedBrushes() {
            m_selectedGeometryDataValid = false;
        }

        void MapRenderer::invalidateAll() {
            invalidateEntities();
            invalidateBrushes();
        }

        void MapRenderer::invalidateEntityModelRendererCache() {
            m_entityRenderer->invalidateModels();
            m_selectedEntityRenderer->invalidateModels();
            m_lockedEntityRenderer->invalidateModels();
        }

        void MapRenderer::addFigure(Figure* figure) {
            m_figures.push_back(figure);
        }
        
        void MapRenderer::removeFigure(Figure* figure) {
            m_figures.erase(std::remove(m_figures.begin(), m_figures.end(), figure), m_figures.end());
        }
        
        void MapRenderer::deleteFigure(Figure* figure) {
            removeFigure(figure);
            m_deletedFigures.push_back(figure);
        }

        void MapRenderer::render(RenderContext& context) {
            if (m_rendering)
                return;
            m_rendering = true;
            
            if (!m_deletedFigures.empty())
                deleteFigures(m_deletedFigures);
            
            validate(context);
            
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glFrontFace(GL_CW);
            glEnable(GL_CULL_FACE);
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
            glShadeModel(GL_SMOOTH);
            glResetEdgeOffset();
            
            if (context.viewOptions().showBrushes() && context.viewOptions().faceRenderMode() != View::ViewOptions::Discard)
                renderFaces(context);
            if (context.viewOptions().showBrushes() && context.viewOptions().renderEdges())
                renderEdges(context);
            
            if (context.viewOptions().showEntities()) {
                m_entityRenderer->render(context);
                if (context.viewOptions().renderSelection())
                    m_selectedEntityRenderer->render(context);
                m_lockedEntityRenderer->render(context);
            }
            
            renderFigures(context);
            
            m_rendering = false;
        }
    }
}