#ifndef TENSORFLOW_SERVING_MODEL_SERVERS_GET_MODEL_RELOAD_STATUS_IMPL_H
#define TENSORFLOW_SERVING_MODEL_SERVERS_GET_MODEL_RELOAD_STATUS_IMPL_H

#include "tensorflow/core/lib/core/status.h"
#include "tensorflow_serving/apis/multipart_file_param.pb.h"
#include "tensorflow_serving/apis/base_response.pb.h"
#include "tensorflow_serving/model_servers/server_core.h"

namespace tensorflow {
namespace serving {
class GetModelReloadStatusImpl {
public:
    static Status GetModelReloadStatus(const ServerCore* core,
                                 const MultipartFileParam& request,
                                 const string model_config_file,
                                 BaseResponse* response);
    static const string RELOAD_SUCCESS;
    static const string RELOAD_ERROR;

private:
    static Status GetNewVersionStatus(const ServerCore* core,
                                      //const MultipartFileParam& request,
                                      const string file_name,
                                      const string file_path,
                                      const string model_config_file);
    static Status GetNewModelStatus(const ServerCore* core,
                                    //const MultipartFileParam& request,
                                    const string file_name,
                                    const string file_path);
    static void SetModelReloadStatusToResponse(const Status& status, BaseResponse* response);
};
}
}
#endif //TENSORFLOW_SERVING_MODEL_SERVERS_GET_MODEL_RELOAD_STATUS_IMPL_H
