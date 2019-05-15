#include <octomap_flatter.h>
#include <octomap_msgs/conversions.h>
#include <octomap/ColorOcTree.h>
#include <octomap/OcTree.h>
#include <cstdint>

#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <visualization_msgs/Marker.h>

namespace octflat
{
OctomapFlatter::OctomapFlatter(ros::NodeHandle &nh, ros::NodeHandle &nh_private) : nh_(nh),
                                                                                   nh_private_(nh_private),
                                                                                   octomap_sub_(nh, "/octomap_full", 1),
                                                                                   projected_map_sub_(nh, "/projected_map", 1),
                                                                                   synchronizer_(SyncPolicy(10), octomap_sub_, projected_map_sub_),
                                                                                   config_()
{
    /* Start Tracking */
    /* In */
    connection_ = synchronizer_.registerCallback(boost::bind(&OctomapFlatter::octomapCallback, this, _1, _2));
    /* Out */
    octomap_pub_ = nh.advertise<octomap_msgs::Octomap>("/octomap_flattened", 10);
    bounding_box_pub_ = nh.advertise<visualization_msgs::Marker>("/octomap_flatter_bounding_box", 10);
    height_image_pub_ = nh.advertise<sensor_msgs::Image>("/height_image", 10);
    /* Parameters */
    dynamic_reconfigure::Server<octomap_flatter::OctoFlatterConfig>::CallbackType f;
    f = boost::bind(&OctomapFlatter::dynamicParameterCallback, this, _1, _2);
    param_server_.setCallback(f);

    ROS_INFO("started octomap_flatter ...");
}

void OctomapFlatter::octomapCallback(const octomap_msgs::Octomap::ConstPtr &octomap_msg,
                                     const nav_msgs::OccupancyGrid::ConstPtr &occupancy_grid_msg)
{
    ROS_INFO("Flattening Octomap");
    // http://docs.ros.org/kinetic/api/octomap_msgs/html/msg/Octomap.html
    octomap::AbstractOcTree *m_abstract_octomap = octomap_msgs::fullMsgToMap(*octomap_msg);
    octomap::OcTree *m_octomap = static_cast<octomap::OcTree *>(m_abstract_octomap);
    ROS_INFO_STREAM("Received tree with " << m_octomap->calcNumNodes() << " nodes");

    double min_Z = std::numeric_limits<double>::max();

    for (octomap::OcTree::leaf_iterator it = m_octomap->begin_leafs(), end = m_octomap->end_leafs(); it != end; ++it)
    {
        if (it.getZ() >= min_Z)
            continue;

        min_Z = it.getZ();
    }

    ROS_INFO_STREAM("min_Z: " << min_Z);

    tf::StampedTransform transform;
    try
    {
        transform_listener_.lookupTransform("/world", "/base_link_estimate",
                                            octomap_msg->header.stamp, transform);
    }
    catch (tf::TransformException ex)
    {
        ROS_ERROR("%s", ex.what());
        ros::Duration(1.0).sleep();
        return;
    }

    /* Construct Height Image */
    // - Translation: [0.267921, 2.43532, 2.16394]
    // - Rotation: in+ Quaternion [-0.286553, 0.576745, -0.163858, 0.747263]

    ROS_INFO_STREAM("Time Stamp: " << octomap_msg->header.stamp);

    // std::cout << transform << std::endl;
    tf::Vector3 v = transform.getOrigin();
    tf::Quaternion q = transform.getRotation();
    std::cout << "- Translation: [" << v.getX() << ", " << v.getY() << ", " << v.getZ() << "]" << std::endl;
    std::cout << "- Rotation: in Quaternion [" << q.getX() << ", " << q.getY() << ", "
              << q.getZ() << ", " << q.getW() << "]" << std::endl;

    // Roll: Around X-Axis
    // Pitch: Around Y-Axis
    // Yaw: Around Z-Axis
    double roll, pitch, yaw;
    transform.getBasis().getRPY(roll, pitch, yaw);
    std::cout << "- Rotation: in Quaternion [" << q.getX() << ", " << q.getY() << ", "
              << q.getZ() << ", " << q.getW() << "]" << std::endl
              << "            in RPY (radian) [" << roll << ", " << pitch << ", " << yaw << "]" << std::endl
              << "            in RPY (degree) [" << roll * 180.0 / M_PI << ", " << pitch * 180.0 / M_PI << ", " << yaw * 180.0 / M_PI << "]" << std::endl;

    // TODO: Get from config
    double flattening_width = 1.0;
    double flattening_height = 2.0;

    tf::Vector3 z_axis(0., 0., 1.);
    tf::Vector3 pt1 = v + tf::Vector3(0.0, flattening_width/2, 0.0).rotate(z_axis, yaw);
    tf::Vector3 pt2 = v + tf::Vector3(0.0, -flattening_width/2, 0.0).rotate(z_axis, yaw);
    tf::Vector3 pt3 = v + tf::Vector3(flattening_height, flattening_width/2, 0.0).rotate(z_axis, yaw);
    tf::Vector3 pt4 = v + tf::Vector3(flattening_height, -flattening_width/2, 0.0).rotate(z_axis, yaw);
    
    double x_min = std::min({pt1.getX(),pt2.getX(),pt3.getX(),pt4.getX()});
    double x_max = std::max({pt1.getX(),pt2.getX(),pt3.getX(),pt4.getX()});
    double y_min = std::min({pt1.getY(),pt2.getY(),pt3.getY(),pt4.getY()});
    double y_max = std::max({pt1.getY(),pt2.getY(),pt3.getY(),pt4.getY()});

    octomap::point3d start_box(x_min, y_min, 0); // -v.getZ() // Get threshold from somewhere else
    octomap::point3d end_box(x_max, y_max, v.getZ());

    ROS_INFO_STREAM("Bounding box: (" << x_min << ", " << y_min << ") --> (" << x_max << ", " << y_max << ")");

    publish_bounding_box(start_box, end_box, octomap_msg->header.stamp);

    /* Create data array */
    // TODO Check for correct frame direction
    double resolution = m_octomap->getResolution();
    uint32_t image_width = (end_box.x() - start_box.x()) / resolution;
    uint32_t image_height = (end_box.y() - start_box.y()) / resolution;

    ROS_INFO_STREAM("Image Width: " << image_width << " Image Height: " << image_height << " Res: " << resolution);

    // /* Taken from sensor_msgs/Images documentation: uint8[] data # actual matrix data, size is (step * rows) */
    std::vector<uint8_t> data(image_width * image_height, 0);
    int full_cnt = 0;
    int cnt = 0;
    for (octomap::OcTree::leaf_bbx_iterator it = m_octomap->begin_leafs_bbx(start_box, end_box), end = m_octomap->end_leafs_bbx(); it != end; ++it)
    {
        full_cnt ++;
        if (m_octomap->search(it.getCoordinate())->getOccupancy() < m_octomap->getOccupancyThres())
            continue;
        cnt ++;
        
        int x = (it.getX() - start_box.x()) / resolution;
        int y = (it.getY() - start_box.y()) / resolution;
        int image_idx = y * image_width + x;

        /* If coordinates are out-of-bounds */
        if (image_idx >= image_width * image_height) {
            // ROS_INFO_STREAM("Out of Bounds x: " << x << " y: " << y << std::endl << "in width: " << image_width << " height: " << image_height);
            continue;
        }
        /* Normalize Z Value in box and scale up to 255 
         * We need to add the resolution as the centers might be above the camera
        */
        // uint8_t z = (it.getZ() - start_box.z()) / (end_box.z() + resolution - start_box.z()) * 255;
        uint8_t z = it.getZ() / (end_box.z() + resolution) * 255;

        /* "+z" to actually print it (uint8_t is typedef char* --> no + results in as interpreting as a char*) */
        // ROS_INFO_STREAM("x, y: " << x << "x" << y << " z: " << +z); 
        data[image_idx] = std::max(data[image_idx], z);
    }
    ROS_INFO_STREAM("Out of " << full_cnt << ", " << cnt << " chosen");

    /*
    Header header        # Header timestamp should be acquisition time of image
                        # Header frame_id should be optical frame of camera
                        # origin of frame should be optical center of camera
                        # +x should point to the right in the image
                        # +y should point down in the image
                        # +z should point into to plane of the image
                        # If the frame_id here and the frame_id of the CameraInfo
                        # message associated with the image conflict
                        # the behavior is undefined

    uint32 height         # image height, that is, number of rows
    uint32 width          # image width, that is, number of columns

    # The legal values for encoding are in file src/image_encodings.cpp
    # If you want to standardize a new string format, join
    # ros-users@lists.sourceforge.net and send an email proposing a new encoding.

    string encoding       # Encoding of pixels -- channel meaning, ordering, size
                        # taken from the list of strings in include/sensor_msgs/image_encodings.h

    uint8 is_bigendian    # is this data bigendian?
    uint32 step           # Full row length in bytes
    uint8[] data          # actual matrix data, size is (step * rows)
    */
    sensor_msgs::Image service_input;
    service_input.header = octomap_msg->header;
    service_input.height = image_height;
    service_input.width = image_width;
    service_input.encoding = sensor_msgs::image_encodings::MONO8;
    service_input.is_bigendian = 0;
    service_input.step = image_width;
    service_input.data.assign(data.begin(), data.end());
    ROS_INFO_STREAM("data size: " << service_input.data.size() << " should be: " << image_width * image_height);
    height_image_pub_.publish(service_input);


    // Send height_image to the service and get the new_image
    ros::ServiceClient client = nh.serviceClient<octomap_flatter::OctoImage>("flatten_octomap");
    octomap_flatter::OctoImage srv;

    srv.request.input = service_input;
    if (client.call(srv))
    {
        sensor_msgs::Image service_output = srv.response.output;
        ROS_INFO_STREAM("Service returned image of size " << service_output.height << " x " << service_output.width);
    }
    else
    {
        ROS_ERROR("Failed to call service add_two_ints");
        return 1;
    }


    // memcpy(&service_input->data, data, sizeof(uint8_t) * image_width * image_height);

    // /* Parallelize this loop for more speed */
    // for (uint y = 0; y < image_height; ++y)
    // {
    //     for (uint x = 0; x < image_width; ++x)
    //     {
    //         array_idx = y * image_width + x;
    //         octomap::KeyRay key_ray;
    //         m_octomap->computeRayKeys(
    //             octomap::point3d(x_min + x * resolution, y_min + y * resolution, v.getZ()),
    //             octomap::point3d(0, 0, -1),
    //             key_ray);
    //         ROS_INFO("------------------------------");
    //         ROS_INFO_STREAM("Key Ray Size: " << key_ray.size());
    //         for (auto key : key_ray)
    //         {
    //             octomap::OcTreeNode* node = m_octomap->search(key);
    //             if (node && node->getOccupancy() > m_octomap->getOccupancyThres())
    //                 ROS_INFO_STREAM("Node: " << node->getOccupancy());
    //         }
    //     }
    // }

    // ROS_INFO_STREAM("Occupancy Threshold: " << m_octomap->getOccupancyThres());


    /* Modify octomap */
    for (octomap::OcTree::leaf_iterator it = m_octomap->begin_leafs(), end = m_octomap->end_leafs(); it != end; ++it)
    {
        // Access various node proprertie e.g.:
        // std::cout << "Node center: " << it.getCoordinate() << std::endl;
        // std::cout << "Node size: " << it.getSize() << std::endl;
        // std::cout << "Node value: " << it->getValue() << std::endl;
        octomap::point3d coord = it.getCoordinate();

        if (coord.z() > min_Z + config_.height_threshold)
            continue;

        octomap::OcTreeKey key = it.getKey();
        // ROS_INFO_STREAM("Key: " << key[0] << " " << key[1] << " " << key[2]);
        /* Delete current node */
        m_octomap->deleteNode(key);

        // ROS_INFO_STREAM("Old coordinate: " << coord);
        // m_octomap->updateNode(key, false, true); // This didn't worked
        /* Push Node to ground */
        coord.z() = min_Z;
        /* Set it filled */
        m_octomap->updateNode(coord, true, true);
    }

    m_octomap->updateInnerOccupancy();
    m_octomap->toMaxLikelihood();
    m_octomap->prune();
    /* Create Message */
    octomap_msgs::Octomap out_msg;
    octomap_msgs::fullMapToMsg(*m_octomap, out_msg);
    out_msg.header = octomap_msg->header;
    // out_msg.header.stamp = ros::Time::now(); // This creates the new octomap at the new timestamp

    /* Publish & Clean up */
    ROS_INFO_STREAM("Publishing tree with " << m_octomap->calcNumNodes() << " nodes");
    octomap_pub_.publish(out_msg);
    delete m_octomap;
}

void OctomapFlatter::dynamicParameterCallback(octomap_flatter::OctoFlatterConfig &config, uint32_t level)
{
    config_ = config;
}

void OctomapFlatter::publish_bounding_box(octomap::point3d start_box, octomap::point3d end_box, ros::Time time_stamp)
{
    visualization_msgs::Marker marker;
    // Set the frame ID and timestamp.  See the TF tutorials for information on these.
    marker.header.frame_id = "/world";
    marker.header.stamp = time_stamp;

    // Set the namespace and id for this marker.  This serves to create a unique ID
    // Any marker sent with the same namespace and id will overwrite the old one
    marker.ns = "octomap_flatter vizualization";
    marker.id = 0;

    // Set the marker type.  Initially this is CUBE, and cycles between that and SPHERE, ARROW, and CYLINDER
    marker.type = visualization_msgs::Marker::CUBE;

    // Set the marker action.  Options are ADD, DELETE, and new in ROS Indigo: 3 (DELETEALL)
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.position.x = (start_box.x() + end_box.x()) / 2;
    marker.pose.position.y = (start_box.y() + end_box.y()) / 2;
    marker.pose.position.z = (start_box.z() + end_box.z()) / 2;
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;

    // Set the scale of the marker -- 1x1x1 here means 1m on a side
    marker.scale.x = end_box.x() - start_box.x();
    marker.scale.y = end_box.y() - start_box.y();
    marker.scale.z = end_box.z() - start_box.z();

    // Set the color -- be sure to set alpha to something non-zero!
    marker.color.r = 0.0f;
    marker.color.g = 1.0f;
    marker.color.b = 0.0f;
    marker.color.a = 0.2;

    marker.lifetime = ros::Duration();

    bounding_box_pub_.publish(marker);
}

} // namespace octflat