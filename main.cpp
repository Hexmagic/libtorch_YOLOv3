#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include "torch/script.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
using namespace std;
/* main */


vector<int> nms(torch::Tensor& boxes, torch::Tensor& scores, torch::Tensor& labels, double conf_thresh, double nms_thresh){
    vector<int> keep;
    if (boxes.numel() == 0)
        return keep;
    auto conf_mask = scores > conf_thresh;

    scores = scores.masked_select(conf_mask);

    conf_mask = conf_mask.unsqueeze(1).expand_as(boxes);
    boxes = boxes.masked_select(conf_mask).reshape({-1,4});

    auto x1 = boxes.select(1, 0).contiguous();
    auto y1 = boxes.select(1, 1).contiguous();
    auto x2 = boxes.select(1, 2).contiguous();
    auto y2 = boxes.select(1, 3).contiguous();
    auto areas = (x2 - x1) * (y2 - y1);
    auto ordered_index = torch::argsort(scores);
    int box_num = scores.numel();

    int suppressed[box_num];
    memset(suppressed, 0, sizeof(suppressed));


    int cnt = 0;
    for (int i = 0;i < box_num;i++){
        int index = ordered_index[i].item<int>();
        if (suppressed[index] == 1)
            continue;
        keep.push_back(index);
        auto ix1 = x1[index];
        auto iy1 = y1[index];
        auto ix2 = x2[index];
        auto iy2 = y2[index];
        auto areai = areas[index].item<double>();


        for (int j = i + 1;j < box_num;j++){
            auto index_ = ordered_index[j].item<int>();
            if (suppressed[index_])
                continue;
            auto jx1 = x1[index_];
            auto jy1 = y1[index_];
            auto jx2 = x2[index_];
            auto jy2 = y2[index_];
            auto inter_tx = max(jx1, ix1);
            auto inter_ty = max(jy1, iy1);
            auto inter_bx = min(jx2, ix2);
            auto inter_by = min(jy2, iy2);
            auto h = max(torch::zeros({1}), inter_by - inter_ty);
            auto w = max(torch::zeros({1}), inter_bx - inter_tx);
            auto inter_area = h * w;
            auto areaj = areas[index_].item<double>() ;
            auto area_inter = inter_area.item<double>();
            auto iou = inter_area / (areai + areaj - inter_area);
            if (iou.item<double>() > nms_thresh){
                suppressed[index_] = 1;
            }
        }
    }
    return keep;
}

class YoloLoss{
    public:
    vector<vector<int>> anchors;
    int num_classes = 80;
    int num_anchors = 3;
    int WIDTH = 416;
    int bbox_atts = 85;
    int HEIGHT = 416;
    double ignore_thrershold = 0.5;
    double lambda_xy = 2.5;
    double lambda_wh = 2.5;
    double lambda_conf = 1.0;
    double lambda_cls = 1.0;

    YoloLoss(vector<vector<int>>& anchors) :anchors(anchors){

    }
    torch::Tensor forward(torch::Tensor& input){
        auto bs = input.size(0);
        auto in_h = input.size(2);
        auto in_w = input.size(3);
        double stride_h = this->HEIGHT / in_h;
        double stride_w = this->WIDTH / in_w;
        vector<double> scaled_w;
        vector<double> scaled_h;
        for (auto& anchor : this->anchors){
            scaled_w.push_back(anchor[0] / stride_w);
            scaled_h.push_back(anchor[1] / stride_h);
        }
        cout << "Input Size" << input.sizes() << endl;
        auto prediction = input.view({bs,this->num_anchors,this->bbox_atts,in_h,in_w}).contiguous();
        auto tmp = prediction.permute({2,0,1,3,4});
        auto x = torch::sigmoid(tmp.select(0, 0));
        auto y = torch::sigmoid(tmp.select(0, 1));
        auto w = tmp.select(0, 2);
        auto h = tmp.select(0, 3);
        auto conf = torch::sigmoid(tmp.select(0, 4).contiguous());
        auto pred_cls = torch::sigmoid(tmp.slice(0, 5, this->bbox_atts)).contiguous();
        auto grid_x = torch::linspace(0, in_w - 1, in_w).repeat({in_w,1}).repeat({bs * this->num_anchors,1,1}).view(x.sizes()).to(torch::kFloat32);
        auto grid_y = torch::linspace(0, in_h - 1, in_h).repeat({in_h,1}).t().repeat({bs * this->num_anchors,1,1}).view(y.sizes()).to(torch::kFloat32);        
        auto anchor_w = torch::tensor(scaled_w).reshape({this->num_anchors,1});
        auto anchor_h = torch::tensor(scaled_h).reshape({this->num_anchors,1});
        anchor_w = anchor_w.repeat({bs,1}).repeat({1,1,in_h * in_w}).view(w.sizes());
        anchor_h = anchor_h.repeat({bs,1}).repeat({1,1,in_h * in_w}).view(h.sizes());
        auto pred_boxes = torch::zeros({4,bs,this->num_anchors,in_h,in_w});
        cout << pred_boxes.sizes() << endl;
        pred_boxes[0] = x.data() + grid_x;        
        pred_boxes[1] = y.data() + grid_y;
        pred_boxes[2] = torch::exp(w.data()) * anchor_w;               
        pred_boxes[3] = torch::exp(h.data()) * anchor_h;
        pred_boxes = pred_boxes.permute({1,2,3,4,0});
        vector<double> stride_arr = {stride_w,stride_h,stride_w,stride_h};
        auto _scale = torch::tensor(stride_arr);
        conf = conf.view({bs,-1 ,1});
        pred_cls = pred_cls.view({bs,-1,this->num_classes});
        pred_boxes = pred_boxes.contiguous().view({bs,-1,4}) * _scale;
        cout << "Cat Tensor: Box Size " << pred_boxes.sizes() << " Conf size: " << conf.sizes() << " Cls size: " << pred_cls.sizes() << endl;
        auto output = torch::cat({
           pred_boxes,
           conf,
           pred_cls
            }, -1);
        return output;
    }
};
int main(){
    // 默认参数
    string img_path = "data/images/bus.jpg";
    double conf_thresh = 0.5;
    double nms_thresh = 0.45;
    int WIDTH = 416;
    int HEIGHT = 416;
    vector<vector<vector<int>>> anchors = {
        {{116, 90}, {156, 198}, {373, 326}},
        {{30, 61}, {62, 45}, {59, 119}},
        {{10, 13}, {16, 30}, {33, 23}}
    };
    vector<YoloLoss> yolo_losses;
    for (int i = 0;i < 3;i++){
        yolo_losses.push_back(YoloLoss(anchors[i]));
    }
    // 加载模型
    auto module = torch::jit::load("weights/model.pt");

    // 加载处理图片
    auto img = cv::imread(img_path, cv::IMREAD_COLOR);
    cv::resize(img, img, {WIDTH,HEIGHT}, cv::INTER_LINEAR);
    cv::Mat cimg;
    cv::cvtColor(img, cimg, cv::COLOR_BGR2RGB);
    cv::Mat img_float;
    cimg.convertTo(img_float, CV_32F, 1 / 255.0, 0);
    torch::Tensor img_tensor = torch::from_blob(img_float.data, {1,416,416,3});
    img_tensor = img_tensor.permute({0,3,1,2});
    std::vector<torch::IValue> inputs;
    inputs.push_back(img_tensor);

    auto outputs = module(inputs).toTuple();
    // 处理输出 
    std::vector<torch::Tensor> output_list;
    auto first = outputs->elements()[0].toTensor();
    cout << first.sizes() << endl;
    for (int i = 0;i < 3;i++){
        auto loss = yolo_losses[i];
        auto elem = outputs->elements()[i].toTensor();        
        auto out = loss.forward(elem);
        output_list.push_back(out);
    }
    auto output = torch::cat(output_list, 1);
    output = output[0];
    cout << output.sizes() << endl;
    auto boxes = output.slice(1, 0, 4);

    auto copy_box = boxes.clone();
    copy_box.select(1, 0) = boxes.select(1, 0) - boxes.select(1, 2) / 2;
    copy_box.select(1, 1) = boxes.select(1, 1) - boxes.select(1, 3) / 2;
    copy_box.select(1, 2) = boxes.select(1, 0) + boxes.select(1, 2) / 2;
    copy_box.select(1, 3) = boxes.select(1, 1) + boxes.select(1, 3) / 2;
    copy_box = copy_box.clamp(0, INT_MAX);
    cout << copy_box.slice(0, 0, 5) << endl;
    auto scores = output.select(1, 4);
    auto labels = output.slice(1, 5, 85);
    auto keep = nms(copy_box, scores, labels, conf_thresh, nms_thresh);

    for (auto& index : keep){
        auto box = copy_box[index];
        auto score = scores[index];
        std::vector<float> box_vec(box.data_ptr<float>(), box.data_ptr<float>() + box.numel());
        vector<int> points;
        for (auto& p : box_vec){
            points.push_back(int(p));
        }
        cv::rectangle(img, cv::Rect(cv::Point(points[0], points[1]), cv::Point(points[2], points[3])), cv::FONT_HERSHEY_PLAIN, 2);
        cout << endl;
    }
    cv::imshow("Test", img);
    cv::waitKey(0);
}
