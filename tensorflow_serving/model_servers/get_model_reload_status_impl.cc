#include "tensorflow_serving/model_servers/get_model_reload_status_impl.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sstream>
#include <fstream>

#include "re2/re2.h"
#include "tensorflow_serving/core/servable_state.h"
#include "tensorflow_serving/core/servable_state_monitor.h"
#include "tensorflow_serving/util/status.pb.h"
#include "tensorflow_serving/util/status_util.h"

namespace tensorflow {
namespace serving {

const string GetModelReloadStatusImpl::RELOAD_SUCCESS = "00";
const string GetModelReloadStatusImpl::RELOAD_ERROR = "09";

namespace {

// Converts ManagerState to enum State in GetModelStatusResponse.
Status ManagerStateToReturnStatus(const ServableState::ManagerState& manager_state) {
    switch (manager_state) {
        case ServableState::ManagerState::kStart: {
            return Status(error::Code::UNKNOWN, "model version status is START");
        }
        case ServableState::ManagerState::kLoading: {
            return Status(error::Code::UNKNOWN, "model version status is LOADING");
        }
        case ServableState::ManagerState::kAvailable: {
            return tensorflow::Status::OK();
        }
        case ServableState::ManagerState::kUnloading: {
            return Status(error::Code::UNKNOWN, "model version status is UNLOADING");
        }
        case ServableState::ManagerState::kEnd: {
            return Status(error::Code::UNKNOWN, "model version status is END");
        }
    }
}

bool ends_with(const string &s, const string &suffix) {
    if (suffix.empty()) {
        return true;
    }
    if (s.empty()) {
        return false;
    }
    return s.rfind(suffix) == s.size() - suffix.size();
}

void split(const string &s, std::vector <string> &sv, const char flag = ' ') {
    sv.clear();
    std::istringstream iss(s);
    string temp;

    while (getline(iss, temp, flag)) {
        sv.push_back(temp);
    }
    //stoi?
    return;
}

template <typename ProtoType>
tensorflow::Status ParseProtoTextFile(const string& file, ProtoType* proto) {
    std::unique_ptr<tensorflow::ReadOnlyMemoryRegion> file_data;
    TF_RETURN_IF_ERROR(
            tensorflow::Env::Default()->NewReadOnlyMemoryRegionFromFile(file,
                                                                        &file_data));
    string file_data_str(static_cast<const char*>(file_data->data()),
                         file_data->length());
    if (tensorflow::protobuf::TextFormat::ParseFromString(file_data_str, proto)) {
        return tensorflow::Status::OK();
    } else {
        return tensorflow::errors::InvalidArgument("Invalid protobuf file: '", file,
                                                   "'");
    }
}

}

Status GetModelReloadStatusImpl::GetModelReloadStatus(const ServerCore* core,
                                                      const MultipartFileParam& request,
                                                      const string model_config_file,
                                                      BaseResponse* response) {
    // step 1 get current config loaded <base_path, name>
    Status status;

    // step 2 zip or model_config_file?
    //if (status.ok()) {
    const string file_name = request.name();
    const string file_path = request.file_path();
    LOG(INFO) << "get file:" << file_path << "/" << file_name;

    if (ends_with(file_name, "zip") || ends_with(file_name, "tar")) {//new version for model
        status = GetNewVersionStatus(core, file_name, file_path, model_config_file);

    } else {//model_config_file
        status = GetNewModelStatus(core, file_name, file_path);
    }
    //}
    SetModelReloadStatusToResponse(status, response);
    return tensorflow::Status::OK();
}

Status GetModelReloadStatusImpl::GetNewVersionStatus(const ServerCore* core,
                                                    //const MultipartFileParam& request,
                                                     const string file_name,
                                                     const string file_path,
                                                     const string model_config_file) {
    //zip或tar包，需要通过路径拼接model_name，然后通过model_name获取当前model，最后获得加载的version
    // step 1 通过file_name获取version
    string model_version;
    string model_postfix;
    std::vector<string> upload_file;
    split(file_name, upload_file, '.');
    if (upload_file.size() != 2) {
        return tensorflow::errors::InvalidArgument("Invalid upload file ", file_name);
    }
    //model_version, model_postfix = file_name.split(".");
    model_version = upload_file[0];
    model_postfix = upload_file[1];
    LOG(INFO) << "get target model version: " << model_version << " and model format: " << model_postfix;

    if (!RE2::FullMatch(string(model_version), R"([0-9]+)")) {
        LOG(ERROR) << "upload model file name is not number :" << model_version;
        return tensorflow::errors::InvalidArgument("upload model file name is not number : ", model_version);
    }

    // step 2 查看zip或tar解压后的目录（<file_full_path>/<version>）是否存在
    const string model_version_path = io::JoinPath(file_path, model_version);
    if (!Env::Default()->FileExists(model_version_path).ok()) {
        return errors::InvalidArgument("model path not exist:", model_version_path);
    }
    // step 3 查看这个路径对应着的model name
    //std::unordered_map<string, string> config_map;
    ModelServerConfig config;
    const Status read_status =
            ParseProtoTextFile<ModelServerConfig>(model_config_file, &config);
    if (!read_status.ok()) {
        LOG(ERROR) << "Failed to read ModelServerConfig file: "
                   << read_status.error_message();
        return Status(error::Code::UNKNOWN, "Failed to read ModelServerConfig file:" + model_config_file);
    }

    string model_name = "";
    for (const ModelConfig& model_config : config.model_config_list().config()) {
        //config_map.insert(std::make_pair(model_config.base_path(), model_config.name()));
        if (model_config.base_path().compare(file_path) == 0) {
            model_name = model_config.name();
        }
    }
    //if (config_map.find(file_path) == config_map.end()) {
    if (model_name.compare("") == 0) {
        return tensorflow::errors::NotFound(
                "Could not find any model name of base_path ", file_path);
    }

    //string model_name = config_map.at(file_path);

    //int target_version = static_cast<int>(model_version);
    int target_version = std::stoi(model_version);
    // step 4 找到model_name已经load进来的versions
    const ServableStateMonitor& monitor = *core->servable_state_monitor();
    const ServableStateMonitor::VersionMap versions_and_states =
            monitor.GetVersionStates(model_name);
    if (versions_and_states.empty()) {
        return tensorflow::errors::NotFound(
                "Could not find any versions of model ", model_name);
    }

    // step 5 找到目标version
    for (const auto& version_and_state : versions_and_states) {
        if (version_and_state.first == target_version) {
            return ManagerStateToReturnStatus(version_and_state.second.state.manager_state);
        }
    }
    // step 6 如果没找到目标version
    return Status(error::Code::UNKNOWN, "model version not found in memory");
}

Status GetModelReloadStatusImpl::GetNewModelStatus(const ServerCore* core,
                                                    //const MultipartFileParam& request,
                                                   const string file_name,
                                                   const string file_path) {
    // step 1 判断文件是否存在
    const string file = io::JoinPath(file_path, file_name);
    if (!Env::Default()->FileExists(file).ok()) {
        return errors::InvalidArgument("file not exist: ", file);
    }

    // step 2 解析上传的model_config_file
    ModelServerConfig proto;
    std::unique_ptr<tensorflow::ReadOnlyMemoryRegion> file_data;
    TF_RETURN_IF_ERROR(tensorflow::Env::Default()->NewReadOnlyMemoryRegionFromFile(file,
                                                                                   &file_data));
    string file_data_str(static_cast<const char*>(file_data->data()),
                         file_data->length());
    if (!tensorflow::protobuf::TextFormat::ParseFromString(file_data_str, &proto)) {
        return tensorflow::errors::InvalidArgument("Invalid protobuf file: '", file, "'");
    }

    // step 3 获取上传的模型配置文件内配置的model
    std::vector<string> target_name_list;
    switch (proto.config_case()) {
        case ModelServerConfig::kModelConfigList: {
            const ModelConfigList list = proto.model_config_list();

            for (int index = 0; index < list.config_size(); index++) {
                const ModelConfig config = list.config(index);
                target_name_list.push_back(config.name());
            }
            //status = core_->ReloadConfig(server_config);
            break;
        }
        default:
            return tensorflow::errors::InvalidArgument(
                    "ServerModelConfig type not supported by GetModelReloadStatus."
                    " Only ModelConfigList is currently supported");
    }

    // step 4 查看所有model_name是否已经load进来
    const ServableStateMonitor& monitor = *core->servable_state_monitor();
    size_t len = target_name_list.size();
    for (size_t i = 0; i < len; ++i) {
        string target_name = target_name_list[i];

        const ServableStateMonitor::VersionMap versions_and_states =
                monitor.GetVersionStates(target_name);
        if (versions_and_states.empty()) {
            return tensorflow::errors::NotFound(
                    "Could not find any versions of model ", target_name);
        }

        bool loaded = false;
        //查看该model的所有versions，只要有一个version是ServableState::ManagerState::kAvailable的即可
        for (const auto& version_and_state : versions_and_states) {
            if (version_and_state.second.state.manager_state == ServableState::ManagerState::kAvailable) {
                //loaded_version = version_and_state.first;
                loaded = true;
                break;
            }
        }
        if (!loaded) {//只要有一个不存在就退出循环
            return Status(error::Code::UNKNOWN, "model " + target_name + " not loaded yet");
        }
    }
    return tensorflow::Status::OK();
}


// Set ModelReloadStatus to BaseResponse
void GetModelReloadStatusImpl::SetModelReloadStatusToResponse(const Status& status, BaseResponse* response) {
    if (status.ok()) {
        response->set_code(GetModelReloadStatusImpl::RELOAD_SUCCESS);
        response->set_msg("reload success");
    } else {
        response->set_code(GetModelReloadStatusImpl::RELOAD_ERROR);
        response->set_msg(status.error_message());
    }
}

}  // namespace serving
}  // namespace tensorflow
