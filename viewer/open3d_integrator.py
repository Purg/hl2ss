#------------------------------------------------------------------------------
# RGBD integration using Open3D. Press space to stop.
#------------------------------------------------------------------------------

from pynput import keyboard

import multiprocessing as mp
import open3d as o3d
import cv2
import hl2ss
import hl2ss_utilities
import hl2ss_3dcv

# Settings --------------------------------------------------------------------

# HoloLens address
host = '192.168.1.7'

# Calibration path
calibration_path = '../calibration'

# Buffer length in seconds
buffer_length = 10

# Integration parameters
voxel_length = 1/100
sdf_trunc = 0.04
max_depth = 3.0

#------------------------------------------------------------------------------

if __name__ == '__main__':
    enable = True

    def on_press(key):
        global enable
        enable = key != keyboard.Key.space
        return enable

    listener = keyboard.Listener(on_press=on_press)
    listener.start()

    calibration_lt = hl2ss_3dcv.get_calibration_rm(host, hl2ss.StreamPort.RM_DEPTH_LONGTHROW, calibration_path)

    uv2xy = hl2ss_3dcv.compute_uv2xy(calibration_lt.intrinsics, hl2ss.Parameters_RM_DEPTH_LONGTHROW.WIDTH, hl2ss.Parameters_RM_DEPTH_LONGTHROW.HEIGHT)
    xy1, scale, _ = hl2ss_3dcv.rm_depth_registration(uv2xy, calibration_lt.scale, calibration_lt.extrinsics, calibration_lt.intrinsics, calibration_lt.intrinsics)
    
    volume = o3d.pipelines.integration.ScalableTSDFVolume(voxel_length=voxel_length, sdf_trunc=sdf_trunc, color_type=o3d.pipelines.integration.TSDFVolumeColorType.RGB8)
    intrinsics_depth = o3d.camera.PinholeCameraIntrinsic(hl2ss.Parameters_RM_DEPTH_LONGTHROW.WIDTH, hl2ss.Parameters_RM_DEPTH_LONGTHROW.HEIGHT, calibration_lt.intrinsics[0, 0], calibration_lt.intrinsics[1, 1], calibration_lt.intrinsics[2, 0], calibration_lt.intrinsics[2, 1])
    vis = o3d.visualization.Visualizer()
    vis.create_window()
    first_pcd = True

    producer = hl2ss_utilities.producer()
    producer.initialize_decoded_rm_depth(hl2ss.Parameters_RM_DEPTH_LONGTHROW.FPS * buffer_length, host, hl2ss.StreamPort.RM_DEPTH_LONGTHROW, hl2ss.ChunkSize.RM_DEPTH_LONGTHROW, hl2ss.StreamMode.MODE_1)
    producer.start()

    manager = mp.Manager()
    consumer = hl2ss_utilities.consumer()
    sink_depth = consumer.create_sink(producer, hl2ss.StreamPort.RM_DEPTH_LONGTHROW, manager, ...)

    sinks = [sink_depth]
    
    [sink.get_attach_response() for sink in sinks]

    while (enable):
        sink_depth.acquire()

        data_depth = sink_depth.get_most_recent_frame()
        if (not data_depth.is_valid_pose()):
            continue

        depth = hl2ss_3dcv.rm_depth_normalize(data_depth.payload.depth, calibration_lt.undistort_map, scale)
        rgb = cv2.remap(data_depth.payload.ab, calibration_lt.undistort_map[:, :, 0], calibration_lt.undistort_map[:, :, 1], cv2.INTER_LINEAR)
        rgb, depth = hl2ss_3dcv.rm_depth_rgbd(depth, rgb)
        rgb = hl2ss_3dcv.rm_depth_ab_to_uint8(rgb)
        rgb = hl2ss_3dcv.rm_vlc_to_rgb(rgb)
        rgbd = o3d.geometry.RGBDImage.create_from_color_and_depth(o3d.geometry.Image(rgb), o3d.geometry.Image(depth), depth_scale=1, depth_trunc=max_depth, convert_rgb_to_intensity=False)
        depth_world_to_camera = hl2ss_3dcv.world_to_reference(data_depth.pose) @ hl2ss_3dcv.rignode_to_camera(calibration_lt.extrinsics)

        volume.integrate(rgbd, intrinsics_depth, depth_world_to_camera.transpose())
        pcd_tmp = volume.extract_point_cloud()

        if (first_pcd):
            first_pcd = False
            pcd = pcd_tmp
            vis.add_geometry(pcd)
        else:
            pcd.points = pcd_tmp.points
            pcd.colors = pcd_tmp.colors
            vis.update_geometry(pcd)

        vis.poll_events()
        vis.update_renderer()

    [sink.detach() for sink in sinks]
    producer.stop()
    listener.join()

    vis.run()
