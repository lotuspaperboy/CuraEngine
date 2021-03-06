
#include "FffGcodeWriter.h"
#include "Progress.h"

namespace cura
{


void FffGcodeWriter::writeGCode(SliceDataStorage& storage, TimeKeeper& timeKeeper)
{
    gcode.preSetup(*this);
    
    gcode.resetTotalPrintTime();
    
    if (commandSocket)
        commandSocket->beginGCode();

    setConfigCoasting();

    setConfigRetraction(storage);

    if (fileNr == 1)
    {
        processStartingCode(storage);
    }
    else
    {
        processNextPrintObjectCode(storage);
    }
    fileNr++;

    // Reoder storage so as to print all layer parts individually 
    if (hasSetting ("StackLayerParts") && getSettingBoolean("StackLayerParts"))
    {   
        isStackLayerParts = true;
        stackLayerParts2 (storage);
    }

    // Reorder storage so as to print all meshes individually
    if (hasSetting ("PrintMeshesSeperatly") && getSettingBoolean("PrintMeshesSeperatly"))
    {
        isMergeMeshes = true;
        mergeMeshes (storage);
    }

    // Check make sure 
    if (isMergeMeshes || isStackLayerParts)
    {
        int nozzleSize = getSettingInMicrons("machine_nozzle_gantry_distance");
        int maxSize = storage.model_max.z;
        if (nozzleSize < maxSize)
        {
            std::cout << "ERROR: MAX POINT OF OBJECT (" << maxSize << ") IS LARGER THAN "
                         "MACHINE NOZZLE SIZE (" << nozzleSize << ")" << std::endl;
            exit(0);
        }
    }

    unsigned int totalLayers = storage.meshes[0].layers.size();

    //gcode.writeComment("Layer count: %d", totalLayers);

    bool has_raft = getSettingAsPlatformAdhesion("adhesion_type") == Adhesion_Raft;

    if (has_raft)
    {
        processRaft(storage, totalLayers);
    }

    layerCount = 0;

    for(unsigned int layer_nr=0; layer_nr<totalLayers; layer_nr++)
    {
        processLayer(storage, layer_nr, totalLayers, has_raft);
    }

    gcode.writeRetraction(&storage.retraction_config, true);

    Progress::messageProgressStage(Progress::Stage::FINISH, &timeKeeper, commandSocket);
    
    gcode.writeFanCommand(0);

    //Store the object height for when we are printing multiple objects, as we need to clear every one of them when moving to the next position.
    maxObjectHeight = std::max(maxObjectHeight, storage.model_max.z);

    if (commandSocket)
    {
        finalize();
        commandSocket->sendGCodeLayer();
        commandSocket->endSendSlicedObject();
        if (gcode.getFlavor() == GCODE_FLAVOR_ULTIGCODE)
        {
            std::ostringstream prefix;
            prefix << ";FLAVOR:UltiGCode\n";
            prefix << ";TIME:" << int(gcode.getTotalPrintTime()) << "\n";
            prefix << ";MATERIAL:" << int(gcode.getTotalFilamentUsed(0)) << "\n";
            prefix << ";MATERIAL2:" << int(gcode.getTotalFilamentUsed(1)) << "\n";
            commandSocket->sendGCodePrefix(prefix.str());
        }
    }
}


void FffGcodeWriter::setConfigCoasting() 
{
    coasting_config.coasting_enable = getSettingBoolean("coasting_enable"); 
    coasting_config.coasting_volume_move = getSettingInCubicMillimeters("coasting_volume_move"); 
    coasting_config.coasting_speed_move = getSettingInCubicMillimeters("coasting_speed_move"); 
    coasting_config.coasting_min_volume_move = getSettingInCubicMillimeters("coasting_min_volume_move"); 

    coasting_config.coasting_volume_retract = getSettingInCubicMillimeters("coasting_volume_retract");
    coasting_config.coasting_speed_retract = getSettingInCubicMillimeters("coasting_speed_retract");
    coasting_config.coasting_min_volume_retract = getSettingInCubicMillimeters("coasting_min_volume_retract");
}

void FffGcodeWriter::setConfigRetraction(SliceDataStorage& storage) 
{
    storage.retraction_config.amount = INT2MM(getSettingInMicrons("retraction_amount"));
    storage.retraction_config.primeAmount = INT2MM(getSettingInMicrons("retraction_extra_prime_amount"));
    storage.retraction_config.speed = getSettingInMillimetersPerSecond("retraction_retract_speed");
    storage.retraction_config.primeSpeed = getSettingInMillimetersPerSecond("retraction_prime_speed");
    storage.retraction_config.zHop = getSettingInMicrons("retraction_hop");
    for(SliceMeshStorage& mesh : storage.meshes)
    {
        mesh.retraction_config = storage.retraction_config;
    }
}

void FffGcodeWriter::setConfigSkirt(SliceDataStorage& storage, int layer_thickness)
{
    storage.skirt_config.setSpeed(getSettingInMillimetersPerSecond("skirt_speed"));
    storage.skirt_config.setLineWidth(getSettingInMicrons("skirt_line_width"));
    storage.skirt_config.setFilamentDiameter(getSettingInMicrons("material_diameter"));
    storage.skirt_config.setFlow(getSettingInPercentage("material_flow"));
    storage.skirt_config.setLayerHeight(layer_thickness);
}

void FffGcodeWriter::setConfigSupport(SliceDataStorage& storage, int layer_thickness)
{
    storage.support_config.setLineWidth(getSettingInMicrons("support_line_width"));
    storage.support_config.setSpeed(getSettingInMillimetersPerSecond("speed_support"));
    storage.support_config.setFilamentDiameter(getSettingInMicrons("material_diameter"));
    storage.support_config.setFlow(getSettingInPercentage("material_flow"));
    storage.support_config.setLayerHeight(layer_thickness);
}

void FffGcodeWriter::setConfigInsets(SliceMeshStorage& mesh, int layer_thickness)
{
    mesh.inset0_config.setLineWidth(mesh.settings->getSettingInMicrons("wall_line_width_0"));
    mesh.inset0_config.setSpeed(mesh.settings->getSettingInMillimetersPerSecond("speed_wall_0"));
    mesh.inset0_config.setFilamentDiameter(mesh.settings->getSettingInMicrons("material_diameter"));
    mesh.inset0_config.setFlow(mesh.settings->getSettingInPercentage("material_flow"));
    mesh.inset0_config.setLayerHeight(layer_thickness);

    mesh.insetX_config.setLineWidth(mesh.settings->getSettingInMicrons("wall_line_width_x"));
    mesh.insetX_config.setSpeed(mesh.settings->getSettingInMillimetersPerSecond("speed_wall_x"));
    mesh.insetX_config.setFilamentDiameter(mesh.settings->getSettingInMicrons("material_diameter"));
    mesh.insetX_config.setFlow(mesh.settings->getSettingInPercentage("material_flow"));
    mesh.insetX_config.setLayerHeight(layer_thickness);
}

void FffGcodeWriter::setConfigSkin(SliceMeshStorage& mesh, int layer_thickness)
{
    mesh.skin_config.setLineWidth(mesh.settings->getSettingInMicrons("skin_line_width"));
    mesh.skin_config.setSpeed(mesh.settings->getSettingInMillimetersPerSecond("speed_topbottom"));
    mesh.skin_config.setFilamentDiameter(mesh.settings->getSettingInMicrons("material_diameter"));
    mesh.skin_config.setFlow(mesh.settings->getSettingInPercentage("material_flow"));
    mesh.skin_config.setLayerHeight(layer_thickness);
}

void FffGcodeWriter::setConfigInfill(SliceMeshStorage& mesh, int layer_thickness)
{
    for(unsigned int idx=0; idx<MAX_SPARSE_COMBINE; idx++)
    {
        mesh.infill_config[idx].setLineWidth(mesh.settings->getSettingInMicrons("infill_line_width") * (idx + 1));
        mesh.infill_config[idx].setSpeed(mesh.settings->getSettingInMillimetersPerSecond("speed_infill"));
        mesh.infill_config[idx].setFilamentDiameter(mesh.settings->getSettingInMicrons("material_diameter"));
        mesh.infill_config[idx].setFlow(mesh.settings->getSettingInPercentage("material_flow"));
        mesh.infill_config[idx].setLayerHeight(layer_thickness);
    }
}

void FffGcodeWriter::processStartingCode(SliceDataStorage& storage)
{
    if (gcode.getFlavor() == GCODE_FLAVOR_ULTIGCODE)
    {
        if (!commandSocket)
        {
            gcode.writeCode(";FLAVOR:UltiGCode\n;TIME:666\n;MATERIAL:666\n;MATERIAL2:-1\n");
        }
    }
    else 
    {
        if (hasSetting("material_bed_temperature") && getSettingInDegreeCelsius("material_bed_temperature") > 0)
            gcode.writeBedTemperatureCommand(getSettingInDegreeCelsius("material_bed_temperature"), true);
        
        for(SliceMeshStorage& mesh : storage.meshes)
            if (mesh.settings->hasSetting("material_print_temperature") && mesh.settings->getSettingInDegreeCelsius("material_print_temperature") > 0)
                gcode.writeTemperatureCommand(mesh.settings->getSettingAsIndex("extruder_nr"), mesh.settings->getSettingInDegreeCelsius("material_print_temperature"));
        for(SliceMeshStorage& mesh : storage.meshes)
            if (mesh.settings->hasSetting("material_print_temperature") && mesh.settings->getSettingInDegreeCelsius("material_print_temperature") > 0)
                gcode.writeTemperatureCommand(mesh.settings->getSettingAsIndex("extruder_nr"), mesh.settings->getSettingInDegreeCelsius("material_print_temperature"), true);
        gcode.writeCode(getSettingString("machine_start_gcode").c_str());
    }
    gcode.writeComment("Generated with Cura_SteamEngine " VERSION);
    if (gcode.getFlavor() == GCODE_FLAVOR_BFB)
    {
        gcode.writeComment("enable auto-retraction");
        std::ostringstream tmp;
        tmp << "M227 S" << (getSettingInMicrons("retraction_amount") * 2560 / 1000) << " P" << (getSettingInMicrons("retraction_amount") * 2560 / 1000);
        gcode.writeLine(tmp.str().c_str());
    }
}

void FffGcodeWriter::processNextPrintObjectCode(SliceDataStorage& storage)
{
    gcode.writeFanCommand(0);
    gcode.resetExtrusionValue();
    gcode.setZ(maxObjectHeight + 5000);
    gcode.writeMove(gcode.getPositionXY(), getSettingInMillimetersPerSecond("speed_travel"), 0);
    gcode.writeMove(Point(storage.model_min.x, storage.model_min.y), getSettingInMillimetersPerSecond("speed_travel"), 0);
}
    
void FffGcodeWriter::processRaft(SliceDataStorage& storage, unsigned int totalLayers)
{
    GCodePathConfig raft_base_config(&storage.retraction_config, "SUPPORT");
    raft_base_config.setSpeed(getSettingInMillimetersPerSecond("raft_base_speed"));
    raft_base_config.setLineWidth(getSettingInMicrons("raft_base_linewidth"));
    raft_base_config.setLayerHeight(getSettingInMicrons("raft_base_thickness"));
    raft_base_config.setFilamentDiameter(getSettingInMicrons("material_diameter"));
    raft_base_config.setFlow(getSettingInPercentage("material_flow"));
    GCodePathConfig raft_interface_config(&storage.retraction_config, "SUPPORT");
    raft_interface_config.setSpeed(getSettingInMillimetersPerSecond("raft_interface_speed"));
    raft_interface_config.setLineWidth(getSettingInMicrons("raft_interface_linewidth"));
    raft_interface_config.setLayerHeight(getSettingInMicrons("raft_base_thickness"));
    raft_interface_config.setFilamentDiameter(getSettingInMicrons("material_diameter"));
    raft_interface_config.setFlow(getSettingInPercentage("material_flow"));
    GCodePathConfig raft_surface_config(&storage.retraction_config, "SUPPORT");
    raft_surface_config.setSpeed(getSettingInMillimetersPerSecond("raft_surface_speed"));
    raft_surface_config.setLineWidth(getSettingInMicrons("raft_surface_line_width"));
    raft_surface_config.setLayerHeight(getSettingInMicrons("raft_base_thickness"));
    raft_surface_config.setFilamentDiameter(getSettingInMicrons("material_diameter"));
    raft_surface_config.setFlow(getSettingInPercentage("material_flow"));

    {
        gcode.writeLayerComment(-2);
        gcode.writeComment("RAFT");
        GCodePlanner gcodeLayer(gcode, storage, &storage.retraction_config, coasting_config, getSettingInMillimetersPerSecond("speed_travel"), getSettingInMicrons("retraction_min_travel"), getSettingBoolean("retraction_combing"), 0, getSettingInMicrons("wall_line_width_0"), getSettingBoolean("travel_avoid_other_parts"), getSettingInMicrons("travel_avoid_distance"));
        if (getSettingAsIndex("support_extruder_nr") > 0)
            gcodeLayer.setExtruder(getSettingAsIndex("support_extruder_nr"));
        gcode.setZ(getSettingInMicrons("raft_base_thickness"));
        gcodeLayer.addPolygonsByOptimizer(storage.raftOutline, &raft_base_config);

        Polygons raftLines;
        int offset_from_poly_outline = 0;
        generateLineInfill(storage.raftOutline, offset_from_poly_outline, raftLines, getSettingInMicrons("raft_base_linewidth"), getSettingInMicrons("raft_line_spacing"), getSettingInPercentage("fill_overlap"), 0);
        gcodeLayer.addLinesByOptimizer(raftLines, &raft_base_config);

        gcode.writeFanCommand(getSettingInPercentage("raft_base_fan_speed"));
        gcodeLayer.writeGCode(false, getSettingInMicrons("raft_base_thickness"));
    }

    { 
        gcode.writeLayerComment(-1);
        gcode.writeComment("RAFT");
        GCodePlanner gcodeLayer(gcode, storage, &storage.retraction_config, coasting_config, getSettingInMillimetersPerSecond("speed_travel"), getSettingInMicrons("retraction_min_travel"), getSettingBoolean("retraction_combing"), 0, getSettingInMicrons("wall_line_width_0"), getSettingBoolean("travel_avoid_other_parts"), getSettingInMicrons("travel_avoid_distance"));
        gcode.setZ(getSettingInMicrons("raft_base_thickness") + getSettingInMicrons("raft_interface_thickness"));

        Polygons raftLines;
        int offset_from_poly_outline = 0;
        generateLineInfill(storage.raftOutline, offset_from_poly_outline, raftLines, getSettingInMicrons("raft_interface_line_width"), getSettingInMicrons("raft_interface_line_spacing"), getSettingInPercentage("fill_overlap"), getSettingAsCount("raft_surface_layers") > 0 ? 45 : 90);
        gcodeLayer.addLinesByOptimizer(raftLines, &raft_interface_config);

        gcodeLayer.writeGCode(false, getSettingInMicrons("raft_interface_thickness"));
    }

    for (int raftSurfaceLayer=1; raftSurfaceLayer<=getSettingAsCount("raft_surface_layers"); raftSurfaceLayer++)
    {
        gcode.writeLayerComment(-1);
        gcode.writeComment("RAFT");
        GCodePlanner gcodeLayer(gcode, storage, &storage.retraction_config, coasting_config, getSettingInMillimetersPerSecond("speed_travel"), getSettingInMicrons("retraction_min_travel"), getSettingBoolean("retraction_combing"), 0, getSettingInMicrons("wall_line_width_0"), getSettingBoolean("travel_avoid_other_parts"), getSettingInMicrons("travel_avoid_distance"));
        gcode.setZ(getSettingInMicrons("raft_base_thickness") + getSettingInMicrons("raft_interface_thickness") + getSettingInMicrons("raft_surface_thickness")*raftSurfaceLayer);

        Polygons raftLines;
        int offset_from_poly_outline = 0;
        generateLineInfill(storage.raftOutline, offset_from_poly_outline, raftLines, getSettingInMicrons("raft_surface_line_width"), getSettingInMicrons("raft_surface_line_spacing"), getSettingInPercentage("fill_overlap"), 90 * raftSurfaceLayer);
        gcodeLayer.addLinesByOptimizer(raftLines, &raft_surface_config);

        gcodeLayer.writeGCode(false, getSettingInMicrons("raft_interface_thickness"));
    }
}

void FffGcodeWriter::processLayer(SliceDataStorage& storage, unsigned int layer_nr, unsigned int totalLayers, bool has_raft)
{
    Progress::messageProgress(Progress::Stage::EXPORT, layer_nr+1, totalLayers, commandSocket);

    int layer_thickness = getSettingInMicrons("layer_height");
    if (layer_nr == 0)
    {
        layer_thickness = getSettingInMicrons("layer_height_0");
    }
    
    setConfigSkirt(storage, layer_thickness);

    setConfigSupport(storage, layer_thickness);
    
    for(SliceMeshStorage& mesh : storage.meshes)
    {
        setConfigInsets(mesh, layer_thickness);
        setConfigSkin(mesh, layer_thickness);
        setConfigInfill(mesh, layer_thickness);
    }

    processInitialLayersSpeedup(storage, layer_nr);

    gcode.writeLayerComment(layer_nr);

    GCodePlanner gcodeLayer(gcode, storage, &storage.retraction_config, coasting_config, getSettingInMillimetersPerSecond("speed_travel"), getSettingInMicrons("retraction_min_travel"), getSettingBoolean("retraction_combing"), layer_nr, getSettingInMicrons("wall_line_width_0"), getSettingBoolean("travel_avoid_other_parts"), getSettingInMicrons("travel_avoid_distance"));
    
    if (!getSettingBoolean("retraction_combing")) 
        gcodeLayer.setAlwaysRetract(true);

    processLayerStartPos(storage, layer_nr, has_raft);

    processSkirt(storage, gcodeLayer, layer_nr);

    bool printSupportFirst = (storage.support.generated && getSettingAsIndex("support_extruder_nr") > 0 && getSettingAsIndex("support_extruder_nr") == gcodeLayer.getExtruder());


    if (printSupportFirst)
        addSupportToGCode(storage, gcodeLayer, layer_nr);

    processOozeShield(storage, gcodeLayer, layer_nr);
    //Figure out in which order to print the meshes, do this by looking at the current extruder and preferer the meshes that use that extruder.
    std::vector<SliceMeshStorage*> mesh_order = calculateMeshOrder(storage, gcodeLayer.getExtruder());
    for(SliceMeshStorage* mesh : mesh_order)
    {
        if (getSettingBoolean("magic_polygon_mode"))
        {
            addMeshLayerToGCode_magicPolygonMode(storage, mesh, gcodeLayer, layer_nr);
        }
        else
        {
            addMeshLayerToGCode(storage, mesh, gcodeLayer, layer_nr);
        }
    }

    if (!printSupportFirst)
        addSupportToGCode(storage, gcodeLayer, layer_nr);

    processFanSpeedAndMinimalLayerTime(storage, gcodeLayer, layer_nr);
   
    bool isNewLayer = false;
    if (layer_nr > 0)
            isNewLayer = storage.meshes[0].layers[layer_nr-1].isNewLayer;

    gcode.writeComment("NEW LAYER: " + std::to_string(isNewLayer));

    gcodeLayer.writeGCode(getSettingBoolean("cool_lift_head"), layer_nr > 0 ? getSettingInMicrons("layer_height") : getSettingInMicrons("layer_height_0"), isNewLayer);


    if (commandSocket)
        commandSocket->sendGCodeLayer();
}

void FffGcodeWriter::processInitialLayersSpeedup(SliceDataStorage& storage, unsigned int layer_nr)
{
    int initial_speedup_layers = getSettingAsCount("speed_slowdown_layers");
    if (static_cast<int>(layer_nr) < initial_speedup_layers)
    {
        int initial_layer_speed = getSettingInMillimetersPerSecond("speed_layer_0");
        storage.support_config.smoothSpeed(initial_layer_speed, layer_nr, initial_speedup_layers);
        for(SliceMeshStorage& mesh : storage.meshes)
        {
            mesh.inset0_config.smoothSpeed(initial_layer_speed, layer_nr, initial_speedup_layers);
            mesh.insetX_config.smoothSpeed(initial_layer_speed, layer_nr, initial_speedup_layers);
            mesh.skin_config.smoothSpeed(initial_layer_speed, layer_nr, initial_speedup_layers);
            for(unsigned int idx=0; idx<MAX_SPARSE_COMBINE; idx++)
            {
                mesh.infill_config[idx].smoothSpeed(initial_layer_speed, layer_nr, initial_speedup_layers);
            }
        }
    }
}

void FffGcodeWriter::processLayerStartPos(SliceDataStorage &storage, unsigned int layer_nr, bool has_raft)
{
    // FIXME: If -S is set but -M isn't then this will only take
    // the values from the first mesh, not the second mesh
    bool isNewLayer = storage.meshes[0].layers[layer_nr].isNewLayer;

    // FIXME: FIGURE OUT WHY THE SECOND LAYER NEEDS TO BE TRUE AS WELL
    if (!isNewLayer && layer_nr > 0 && storage.meshes[0].layers[layer_nr-1].isNewLayer)
    {
        isNewLayer = true;
    }

    if (isNewLayer)
    {
        layerCount = 0;
    }

    layerCount++;

    int32_t z = getSettingInMicrons("layer_height_0") + layerCount * getSettingInMicrons("layer_height");

    if (has_raft)
    {
        z += getSettingInMicrons("raft_base_thickness") + getSettingInMicrons("raft_interface_thickness") + getSettingAsCount("raft_surface_layers")*getSettingInMicrons("raft_surface_thickness");
        if (layer_nr == 0)
        {
            z += getSettingInMicrons("raft_airgap_layer_0");
        } else {
            z += getSettingInMicrons("raft_airgap");
        }
    }

    if ((isMergeMeshes || isStackLayerParts) && isNewLayer)
    {
        /*retractHeadSaftly();
        Point p = gcode.getPositionXY();
        Point3 first_point;

        first_point.x = p.X;
        first_point.y = p.Y;
        first_point.z = getSettingInMicrons("machine_height");

        gcode.writeMove(first_point, getSettingInMillimetersPerSecond("retraction_retract_speed"), 0); 
        gcode.writeComment("GOT TO HERE");*/

        gcode.nextZPos = z;
        gcode.setZ(gcode.getPositionZ() + 10000);
    }
    else
    {
        gcode.resetStartPosition();
        gcode.setZ(z);
    }
}

void FffGcodeWriter::processSkirt(SliceDataStorage& storage, GCodePlanner& gcodeLayer, unsigned int layer_nr)
{
    if (layer_nr == 0)
    {
        if (storage.skirt.size() > 0)
            gcodeLayer.addTravel(storage.skirt[storage.skirt.size()-1].closestPointTo(gcode.getPositionXY()));
        gcodeLayer.addPolygonsByOptimizer(storage.skirt, &storage.skirt_config);
    }
}

void FffGcodeWriter::processOozeShield(SliceDataStorage& storage, GCodePlanner& gcodeLayer, unsigned int layer_nr)
{
    if (storage.oozeShield.size() > 0)
    {
        gcodeLayer.setAlwaysRetract(true);
        gcodeLayer.addPolygonsByOptimizer(storage.oozeShield[layer_nr], &storage.skirt_config);
        gcodeLayer.setAlwaysRetract(!getSettingBoolean("retraction_combing"));
    }
}

std::vector<SliceMeshStorage*> FffGcodeWriter::calculateMeshOrder(SliceDataStorage& storage, int current_extruder)
{
    std::vector<SliceMeshStorage*> ret;
    std::vector<SliceMeshStorage*> add_list;
    for(SliceMeshStorage& mesh : storage.meshes)
        add_list.push_back(&mesh);

    int add_extruder_nr = current_extruder;
    while(add_list.size() > 0)
    {
        for(unsigned int idx=0; idx<add_list.size(); idx++)
        {
            if (add_list[idx]->settings->getSettingAsIndex("extruder_nr") == add_extruder_nr)
            {
                ret.push_back(add_list[idx]);
                add_list.erase(add_list.begin() + idx);
                idx--;
            }
        }
        if (add_list.size() > 0)
            add_extruder_nr = add_list[0]->settings->getSettingAsIndex("extruder_nr");
    }
    return ret;
}


void FffGcodeWriter::addMeshLayerToGCode_magicPolygonMode(SliceDataStorage& storage, SliceMeshStorage* mesh, GCodePlanner& gcodeLayer, int layer_nr)
{
    int prevExtruder = gcodeLayer.getExtruder();
    bool extruder_changed = gcodeLayer.setExtruder(mesh->settings->getSettingAsIndex("extruder_nr"));

    if (extruder_changed)
        addWipeTower(storage, gcodeLayer, layer_nr, prevExtruder);

    SliceLayer* layer = &mesh->layers[layer_nr];


    Polygons polygons;
    for(unsigned int partNr=0; partNr<layer->parts.size(); partNr++)
    {
        for(unsigned int n=0; n<layer->parts[partNr].outline.size(); n++)
        {
            for(unsigned int m=1; m<layer->parts[partNr].outline[n].size(); m++)
            {
                Polygon p;
                p.add(layer->parts[partNr].outline[n][m-1]);
                p.add(layer->parts[partNr].outline[n][m]);
                polygons.add(p);
            }
            if (layer->parts[partNr].outline[n].size() > 0)
            {
                Polygon p;
                p.add(layer->parts[partNr].outline[n][layer->parts[partNr].outline[n].size()-1]);
                p.add(layer->parts[partNr].outline[n][0]);
                polygons.add(p);
            }
        }
    }
    for(unsigned int n=0; n<layer->openLines.size(); n++)
    {
        for(unsigned int m=1; m<layer->openLines[n].size(); m++)
        {
            Polygon p;
            p.add(layer->openLines[n][m-1]);
            p.add(layer->openLines[n][m]);
            polygons.add(p);
        }
    }
    if (mesh->settings->getSettingBoolean("magic_spiralize"))
        mesh->inset0_config.spiralize = true;

    gcodeLayer.addPolygonsByOptimizer(polygons, &mesh->inset0_config);
    
}

void FffGcodeWriter::addMeshLayerToGCode(SliceDataStorage& storage, SliceMeshStorage* mesh, GCodePlanner& gcodeLayer, int layer_nr)
{
    int prevExtruder = gcodeLayer.getExtruder();
    bool extruder_changed = gcodeLayer.setExtruder(mesh->settings->getSettingAsIndex("extruder_nr"));

    if (extruder_changed)
        addWipeTower(storage, gcodeLayer, layer_nr, prevExtruder);

    SliceLayer* layer = &mesh->layers[layer_nr];

    PathOrderOptimizer partOrderOptimizer(gcode.getStartPositionXY());
    for(unsigned int partNr=0; partNr<layer->parts.size(); partNr++)
    {
        partOrderOptimizer.addPolygon(layer->parts[partNr].insets[0][0]);
    }
    partOrderOptimizer.optimize();

    bool skin_alternate_rotation = getSettingBoolean("skin_alternate_rotation") && ( getSettingAsCount("top_layers") >= 4 || getSettingAsCount("bottom_layers") >= 4 );
    
    for(int order_idx : partOrderOptimizer.polyOrder)
    {
        SliceLayerPart& part = layer->parts[order_idx];

        int fillAngle = 45;
        if (layer_nr & 1)
            fillAngle += 90;
        int extrusionWidth = getSettingInMicrons("infill_line_width");
        
        int sparse_infill_line_distance = getSettingInMicrons("infill_line_distance");
        double infill_overlap = getSettingInPercentage("fill_overlap");
        
        gcode.writeComment("GOT TO HERE Y");
        if (!storage.meshes[0].layers[layer_nr].isNewLayer) {

        processMultiLayerInfill(gcodeLayer, mesh, part, sparse_infill_line_distance, infill_overlap, fillAngle, extrusionWidth);
        processSingleLayerInfill(gcodeLayer, mesh, part, sparse_infill_line_distance, infill_overlap, fillAngle, extrusionWidth);

        processInsets(gcodeLayer, mesh, part, layer_nr);

        

        if (skin_alternate_rotation && ( layer_nr / 2 ) & 1)
            fillAngle -= 45;
        processSkin(gcodeLayer, mesh, part, layer_nr, infill_overlap, fillAngle, extrusionWidth);

    
        }
        gcode.writeComment("GOT TO HERE YE");

    }
}


void FffGcodeWriter::processMultiLayerInfill(GCodePlanner& gcodeLayer, SliceMeshStorage* mesh, SliceLayerPart& part, int sparse_infill_line_distance, double infill_overlap, int fillAngle, int extrusionWidth)
{
    if (sparse_infill_line_distance > 0)
    {
        //Print the thicker sparse lines first. (double or more layer thickness, infill combined with previous layers)
        for(unsigned int n=1; n<part.sparse_outline.size(); n++)
        {
            Polygons fillPolygons;
            switch(getSettingAsFillMethod("fill_pattern"))
            {
            case Fill_Grid:
                generateGridInfill(part.sparse_outline[n], 0, fillPolygons, extrusionWidth, sparse_infill_line_distance * 2, infill_overlap, fillAngle);
                gcodeLayer.addLinesByOptimizer(fillPolygons, &mesh->infill_config[n]);
                break;
            case Fill_Lines:
                generateLineInfill(part.sparse_outline[n], 0, fillPolygons, extrusionWidth, sparse_infill_line_distance, infill_overlap, fillAngle);
                gcodeLayer.addLinesByOptimizer(fillPolygons, &mesh->infill_config[n]);
                break;
            case Fill_Triangles:
                generateTriangleInfill(part.sparse_outline[n], 0, fillPolygons, extrusionWidth, sparse_infill_line_distance * 3, infill_overlap, 0);
                gcodeLayer.addLinesByOptimizer(fillPolygons, &mesh->infill_config[n]);
                break;
            case Fill_Concentric:
                generateConcentricInfill(part.sparse_outline[n], fillPolygons, sparse_infill_line_distance);
                gcodeLayer.addPolygonsByOptimizer(fillPolygons, &mesh->infill_config[n]);
                break;
            case Fill_ZigZag:
                generateZigZagInfill(part.sparse_outline[n], fillPolygons, extrusionWidth, sparse_infill_line_distance, infill_overlap, fillAngle, false, false);
                gcodeLayer.addPolygonsByOptimizer(fillPolygons, &mesh->infill_config[n]);
                break;
            default:
                logError("fill_pattern has unknown value.\n");
                break;
            }
        }
    }
}

void FffGcodeWriter::processSingleLayerInfill(GCodePlanner& gcodeLayer, SliceMeshStorage* mesh, SliceLayerPart& part, int sparse_infill_line_distance, double infill_overlap, int fillAngle, int extrusionWidth)
{
    //Combine the 1 layer thick infill with the top/bottom skin and print that as one thing.
    Polygons infillPolygons;
    Polygons infillLines;
    if (sparse_infill_line_distance > 0 && part.sparse_outline.size() > 0)
    {
        switch(getSettingAsFillMethod("fill_pattern"))
        {
        case Fill_Grid:
            generateGridInfill(part.sparse_outline[0], 0, infillLines, extrusionWidth, sparse_infill_line_distance * 2, infill_overlap, fillAngle);
            break;
        case Fill_Lines:
            generateLineInfill(part.sparse_outline[0], 0, infillLines, extrusionWidth, sparse_infill_line_distance, infill_overlap, fillAngle);
            break;
        case Fill_Triangles:
            generateTriangleInfill(part.sparse_outline[0], 0, infillLines, extrusionWidth, sparse_infill_line_distance * 3, infill_overlap, 0);
            break;
        case Fill_Concentric:
            generateConcentricInfill(part.sparse_outline[0], infillPolygons, sparse_infill_line_distance);
            break;
        case Fill_ZigZag:
            generateZigZagInfill(part.sparse_outline[0], infillLines, extrusionWidth, sparse_infill_line_distance, infill_overlap, fillAngle, false, false);
            break;
        default:
            logError("fill_pattern has unknown value.\n");
            break;
        }
    }
    gcodeLayer.addPolygonsByOptimizer(infillPolygons, &mesh->infill_config[0]);
    gcodeLayer.addLinesByOptimizer(infillLines, &mesh->infill_config[0]); 
}

void FffGcodeWriter::processInsets(GCodePlanner& gcodeLayer, SliceMeshStorage* mesh, SliceLayerPart& part, unsigned int layer_nr)
{
    if (getSettingAsCount("wall_line_count") > 0)
    {
        if (getSettingBoolean("magic_spiralize"))
        {
            if (static_cast<int>(layer_nr) >= getSettingAsCount("bottom_layers"))
                mesh->inset0_config.spiralize = true;
            if (static_cast<int>(layer_nr) == getSettingAsCount("bottom_layers") && part.insets.size() > 0)
                gcodeLayer.addPolygonsByOptimizer(part.insets[0], &mesh->insetX_config);
        }
        for(int insetNr=part.insets.size()-1; insetNr>-1; insetNr--)
        {
            if (insetNr == 0)
                gcodeLayer.addPolygonsByOptimizer(part.insets[insetNr], &mesh->inset0_config);
            else
                gcodeLayer.addPolygonsByOptimizer(part.insets[insetNr], &mesh->insetX_config);
        }
    }
}


void FffGcodeWriter::processSkin(GCodePlanner& gcodeLayer, SliceMeshStorage* mesh, SliceLayerPart& part, unsigned int layer_nr, double infill_overlap, int fillAngle, int extrusionWidth)
{
    Polygons skinPolygons;
    Polygons skinLines;
    for(SkinPart& skin_part : part.skin_parts)
    {
        int bridge = -1;
        if (layer_nr > 0)
            bridge = bridgeAngle(skin_part.outline, &mesh->layers[layer_nr-1]);
        if (bridge > -1)
        {
            generateLineInfill(skin_part.outline, 0, skinLines, extrusionWidth, extrusionWidth, infill_overlap, bridge);
        }else{
            switch(getSettingAsFillMethod("top_bottom_pattern"))
            {
            case Fill_Lines:
                for (Polygons& skin_perimeter : skin_part.insets)
                {
                    gcodeLayer.addPolygonsByOptimizer(skin_perimeter, &mesh->skin_config); // add polygons to gcode in inward order
                }
                if (skin_part.insets.size() > 0)
                {
                    generateLineInfill(skin_part.insets.back(), -extrusionWidth/2, skinLines, extrusionWidth, extrusionWidth, infill_overlap, fillAngle);
                    if (getSettingString("fill_perimeter_gaps") != "Nowhere")
                    {
                        generateLineInfill(skin_part.perimeterGaps, 0, skinLines, extrusionWidth, extrusionWidth, 0, fillAngle);
                    }
                } 
                else
                {
                    generateLineInfill(skin_part.outline, 0, skinLines, extrusionWidth, extrusionWidth, infill_overlap, fillAngle);
                }
                break;
            case Fill_Concentric:
                {
                    Polygons in_outline;
                    offsetSafe(skin_part.outline, -extrusionWidth/2, extrusionWidth, in_outline, getSettingBoolean("wall_overlap_avoid_enabled"));
                    if (getSettingString("fill_perimeter_gaps") != "Nowhere")
                    {
                        generateConcentricInfillDense(in_outline, skinPolygons, &part.perimeterGaps, extrusionWidth, getSettingBoolean("wall_overlap_avoid_enabled"));
                    }
                }
                break;
            default:
                logError("Unknown fill method for skin\n");
                break;
            }
        }
    }
    
    // handle gaps between perimeters etc.
    if (getSettingString("fill_perimeter_gaps") != "Nowhere")
    {
        generateLineInfill(part.perimeterGaps, 0, skinLines, extrusionWidth, extrusionWidth, 0, fillAngle);
    }
    
    
    gcodeLayer.addPolygonsByOptimizer(skinPolygons, &mesh->skin_config);
    gcodeLayer.addLinesByOptimizer(skinLines, &mesh->skin_config);
}


void FffGcodeWriter::addSupportToGCode(SliceDataStorage& storage, GCodePlanner& gcodeLayer, int layer_nr)
{
    if (!storage.support.generated)
        return;
    
    int support_line_distance = getSettingInMicrons("support_line_distance");
    int extrusionWidth = storage.support_config.getLineWidth();
    double infill_overlap = getSettingInPercentage("fill_overlap");
    EFillMethod support_pattern = getSettingAsFillMethod("support_pattern");
    
    if (getSettingAsIndex("support_extruder_nr") > -1)
    {
        int prevExtruder = gcodeLayer.getExtruder();
        if (gcodeLayer.setExtruder(getSettingAsIndex("support_extruder_nr")))
            addWipeTower(storage, gcodeLayer, layer_nr, prevExtruder);
    }

    Polygons support;

    if (storage.support.generated)
    {
        support = storage.support.supportAreasPerLayer[layer_nr];
    }

    std::vector<PolygonsPart> supportIslands = support.splitIntoParts();

    PathOrderOptimizer islandOrderOptimizer(gcode.getPositionXY());
    for(unsigned int n=0; n<supportIslands.size(); n++)
    {
        islandOrderOptimizer.addPolygon(supportIslands[n][0]);
    }
    islandOrderOptimizer.optimize();

    for(unsigned int n=0; n<supportIslands.size(); n++)
    {
        PolygonsPart& island = supportIslands[islandOrderOptimizer.polyOrder[n]];

        Polygons supportLines;
        if (support_line_distance > 0)
        {
            switch(support_pattern)
            {
            case Fill_Grid:
                {
                    int offset_from_outline = 0;
                    if (support_line_distance > extrusionWidth * 4)
                    {
                        generateGridInfill(island, offset_from_outline, supportLines, extrusionWidth, support_line_distance*2, infill_overlap, 0);
                    }else{
                        generateLineInfill(island, offset_from_outline, supportLines, extrusionWidth, support_line_distance, infill_overlap, (layer_nr & 1) ? 0 : 90);
                    }
                }
                break;
            case Fill_Lines:
                {
                    int offset_from_outline = 0;
                    if (layer_nr == 0)
                    {
                        generateGridInfill(island, offset_from_outline, supportLines, extrusionWidth, support_line_distance, 0 + 150, 0);
                    }else{
                        generateLineInfill(island, offset_from_outline, supportLines, extrusionWidth, support_line_distance, 0, 0);
                    }
                }
                break;
            case Fill_ZigZag:
                {
                    int offset_from_outline = 0;
                    if (layer_nr == 0)
                    {
                        generateGridInfill(island, offset_from_outline, supportLines, extrusionWidth, support_line_distance, 0 + 150, 0);
                    }else{
                        generateZigZagInfill(island, supportLines, extrusionWidth, support_line_distance, 0, 0, getSettingBoolean("support_connect_zigzags"), true);
                    }
                }
                break;
            default:
                logError("Unknown fill method for support\n");
                break;
            }
        }

        if (support_pattern == Fill_Grid || ( support_pattern == Fill_ZigZag && layer_nr == 0 ) )
            gcodeLayer.addPolygonsByOptimizer(island, &storage.support_config);
        gcodeLayer.addLinesByOptimizer(supportLines, &storage.support_config);
    }
}


void FffGcodeWriter::addWipeTower(SliceDataStorage& storage, GCodePlanner& gcodeLayer, int layer_nr, int prev_extruder)
{
    if (getSettingInMicrons("wipe_tower_size") < 1)
        return;

    int64_t offset = -getSettingInMicrons("wall_line_width_x");
    if (layer_nr > 0)
        offset *= 2;
    
    //If we changed extruder, print the wipe/prime tower for this nozzle;
    std::vector<Polygons> insets;
    if ((layer_nr % 2) == 1)
        insets.push_back(storage.wipeTower.offset(offset / 2));
    else
        insets.push_back(storage.wipeTower);
    while(true)
    {
        Polygons new_inset = insets[insets.size() - 1].offset(offset);
        if (new_inset.size() < 1)
            break;
        insets.push_back(new_inset);
    }
    for(unsigned int n=0; n<insets.size(); n++)
    {
        gcodeLayer.addPolygonsByOptimizer(insets[insets.size() - 1 - n], &storage.meshes[0].insetX_config);
    }
    
    //Make sure we wipe the old extruder on the wipe tower.
    gcodeLayer.addTravel(storage.wipePoint - gcode.getExtruderOffset(prev_extruder) + gcode.getExtruderOffset(gcodeLayer.getExtruder()));
}

void FffGcodeWriter::processFanSpeedAndMinimalLayerTime(SliceDataStorage& storage, GCodePlanner& gcodeLayer, unsigned int layer_nr)
{ 
    double travelTime;
    double extrudeTime;
    gcodeLayer.getTimes(travelTime, extrudeTime);
    gcodeLayer.forceMinimalLayerTime(getSettingInSeconds("cool_min_layer_time"), getSettingInMillimetersPerSecond("cool_min_speed"), travelTime, extrudeTime);

    // interpolate fan speed (for cool_fan_full_layer and for cool_min_layer_time_fan_speed_max)
    int fanSpeed = getSettingInPercentage("cool_fan_speed_min");
    double totalLayerTime = travelTime + extrudeTime;
    if (totalLayerTime < getSettingInSeconds("cool_min_layer_time"))
    {
        fanSpeed = getSettingInPercentage("cool_fan_speed_max");
    }
    else if (totalLayerTime < getSettingInSeconds("cool_min_layer_time_fan_speed_max"))
    { 
        // when forceMinimalLayerTime didn't change the extrusionSpeedFactor, we adjust the fan speed
        double minTime = (getSettingInSeconds("cool_min_layer_time"));
        double maxTime = (getSettingInSeconds("cool_min_layer_time_fan_speed_max"));
        int fanSpeedMin = getSettingInPercentage("cool_fan_speed_min");
        int fanSpeedMax = getSettingInPercentage("cool_fan_speed_max");
        fanSpeed = fanSpeedMax - (fanSpeedMax-fanSpeedMin) * (totalLayerTime - minTime) / (maxTime - minTime);
    }
    if (static_cast<int>(layer_nr) < getSettingAsCount("cool_fan_full_layer"))
    {
        //Slow down the fan on the layers below the [cool_fan_full_layer], where layer 0 is speed 0.
        fanSpeed = fanSpeed * layer_nr / getSettingAsCount("cool_fan_full_layer");
    }
    gcode.writeFanCommand(fanSpeed);
}

void FffGcodeWriter::finalize()
{
    gcode.finalize(maxObjectHeight, getSettingInMillimetersPerSecond("speed_travel"), getSettingString("machine_end_gcode").c_str());
    for(int e=0; e<MAX_EXTRUDERS; e++)
        gcode.writeTemperatureCommand(e, 0, false);
}

void FffGcodeWriter::retractHeadSaftly ()
{
    gcode.writeComment("RETRACTING THE HEAD");

    Point p = gcode.getPositionXY();
    Point3 first_point;

    first_point.x = p.X;
    first_point.y = p.Y;
    first_point.z = getSettingInMicrons("machine_height");

    gcode.writeMove(first_point, getSettingInMillimetersPerSecond("retraction_retract_speed"), 0);
    gcode.writeComment("GOT TO HERE");
}

void FffGcodeWriter::stackLayerParts2 (SliceDataStorage &storage)
{
    for (unsigned int mesh_index=0; mesh_index < storage.meshes.size(); mesh_index++)
    {
        std::vector<std::vector<SliceLayer> > storageContainer;

        for (std::vector<SliceLayer>::iterator layer = storage.meshes[mesh_index].layers.begin();
             layer != storage.meshes[mesh_index].layers.end(); layer++)
        {
            for (std::vector<SliceLayerPart>::iterator part = layer->parts.begin();
                part != layer->parts.end(); part++)
            {
                unsigned int index = part - layer->parts.begin();

                if (index == storageContainer.size())
                {
                    storageContainer.push_back(std::vector<SliceLayer>());
                }

                SliceLayer newLayer = *layer;
                newLayer.parts.clear();
                newLayer.parts.push_back(*part);
                storageContainer[index].push_back (newLayer);
            } 
        }

        storage.meshes[mesh_index].layers.clear();

        // ADD SUPPORT
        std::vector<Polygons> supports = storage.support.supportAreasPerLayer;
        for (unsigned int i = 1; i < storageContainer.size(); i++)
        {
            storage.support.supportAreasPerLayer.insert(storage.support.supportAreasPerLayer.end(),
                                                        supports.begin(), supports.end());
        }

        for (std::vector<std::vector<SliceLayer> >::reverse_iterator stack = storageContainer.rbegin();
            stack != storageContainer.rend(); stack++)
        {
            for (std::vector<SliceLayer>::iterator layer = stack->begin(); layer != stack->end(); layer++)
            {
                if (layer == stack->begin() && stack != storageContainer.rbegin())
                {
                    layer->isNewLayer = true;
                }

                storage.meshes[mesh_index].layers.push_back (*layer);
            }
        }
    }
}

void FffGcodeWriter::mergeMeshes(SliceDataStorage &storage)
{
    unsigned int meshCount = storage.meshes.size();
    
    if (meshCount == 1)
    {
            std::cout << "WARNING: -S flag detected but only 1 model loaded" << std::endl;
            return;
    }

    std::vector<SliceLayer> layers;

    for (std::vector<SliceMeshStorage>::iterator mesh = storage.meshes.begin();
         mesh != storage.meshes.end(); mesh++)
    {
        for (std::vector<SliceLayer>::iterator layer = mesh->layers.begin();
             layer != mesh->layers.end(); layer++)
        {
            SliceLayer newLayer = *layer;
        
            if ((layer == mesh->layers.begin() && mesh != storage.meshes.begin()) || newLayer.isNewLayer)
            {
                newLayer.isNewLayer = true;
            }
            
            layers.push_back (newLayer);
        }
    }

    std::vector<Polygons> supports = storage.support.supportAreasPerLayer;

    while (storage.meshes.size() > 1)
    {
        storage.meshes.pop_back();

        storage.support.supportAreasPerLayer.insert(storage.support.supportAreasPerLayer.end(),
                                                    supports.begin(), supports.end());
    }

    storage.meshes[0].layers.clear();

    for (std::vector<SliceLayer>::iterator layer = layers.begin();
         layer != layers.end(); layer++)
    {
        storage.meshes[0].layers.push_back (*layer);
    }    
}

} // namespace cura
