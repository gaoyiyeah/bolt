// Copyright (C) 2019. Huawei Technologies Co., Ltd. All rights reserved.

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), 
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE 
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR 
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#include <iostream>

#include "inference.hpp"
#include "tensor.hpp"
#include "data_loader.hpp"
#include "utils.hpp"

void print_help(char* argv[]) {
    std::cout << "usage: " << argv[0] << " modelPath sequencesDirectory subNetworkName[encoder_decoder|postnet] cpuAffinityPolicyName" << std::endl;
}

HashMap<std::string, Tensor> prepareStates(DataType dt, std::string sequenceDirectory,
    std::string shapeMapFileName)
{
    HashMap<std::string, TensorDesc> shapeMap;
    std::string filePath = sequenceDirectory + "/" + shapeMapFileName;
    FILE *shapeMapFile = fopen(filePath.c_str(), "r");
    char buffer[NAME_LEN];
    while (fscanf(shapeMapFile, "%s", buffer) != EOF) {
        TensorDesc desc;
        fscanf(shapeMapFile, "%u", &(desc.nDims));
        for (U32 i = 0; i < desc.nDims; i++)
            fscanf(shapeMapFile, "%u", &(desc.dims[desc.nDims - 1 - i]));
        if (std::string(buffer) == std::string("tts_words")) {
            desc.dt = DT_U32;
        } else {
            desc.dt = dt;
        }
        if (std::string(buffer) == std::string("tts_words") || std::string(buffer) == std::string("tts_alignments")) {
            desc.df = DF_NORMAL;
        } else {
            desc.df = DF_NCHW;
        }
        shapeMap[buffer] = desc;
    }
    fclose(shapeMapFile);

    HashMap<std::string, Tensor> tensorMap;
    for (auto iter: shapeMap) {
        std::string filePath = sequenceDirectory + "/" + iter.first + ".txt";
        TensorDesc desc = iter.second;
        tensorMap[iter.first] = load_txt(filePath, Vec<TensorDesc>{desc})[0];
    }
    return tensorMap;
}

void saveStates(std::shared_ptr<CNN> pipeline, std::string sequenceDirectory,
    std::string outputFileName, std::string outputStatesFileName)
{
    char buffer[NAME_LEN];
    std::string outputFilePath = sequenceDirectory + "/" + outputFileName;
    std::string outputStatesFilePath = sequenceDirectory + "/" + outputStatesFileName;
    FILE *outputFile = fopen(outputFilePath.c_str(), "r");
    FILE *outputStatesFile = fopen(outputStatesFilePath.c_str(), "w");
    while (!feof(outputFile)) {
        fscanf(outputFile, "%s", buffer);
        Tensor tensor = pipeline->get_tensor_by_name(buffer);
        TensorDesc desc = tensor.get_desc();

        // write states
        fprintf(outputStatesFile, "%s\n", buffer);
        fprintf(outputStatesFile, "%u\n", desc.nDims);
        for (U32 i = 0; i < desc.nDims; i++)
            fprintf(outputStatesFile, "%u ", desc.dims[desc.nDims-1-i]);

        // write data
        U32 num = tensorNumElements(desc);
        std::string outputDataPath = sequenceDirectory + "/" + std::string(buffer) + ".txt";
        FILE *outputDataFile = fopen(outputDataPath.c_str(), "w");
        for (U32 i = 0; i < num; i++) {
            fprintf(outputDataFile, "%f ", tensor.getElement(i));
            if (i % 10 == 9)
                fprintf(outputDataFile, "\n");
        }
        fclose(outputDataFile);
    }
    fclose(outputFile);
    fclose(outputStatesFile);
}

int verify(Tensor tensor, std::string subNetworkName)
{
    U32 num = tensorNumElements(tensor.get_desc());
    F32 sum = 0;
    for (U32 i = 0; i < num; i++) {
        sum += tensor.getElement(i);
    }
    I32 result = 0;
    if (subNetworkName == std::string("encoder_decoder")) {
        if (abs(sum - 6921) >= 1100) {
            result = 1;
        }
    } else if (subNetworkName == std::string("postnet")) {
        if (abs(sum - (-11987.7)) >= 1) {
            result = 1;
        }
    } else if (subNetworkName == std::string("melgan_vocoder")) {
        if (abs(sum - (-0.665192)) >= 0.7) {
            result = 1;
        }
    }
    return result;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        print_help(argv);
        return 1;
    }
    UTIL_TIME_INIT

    char* modelPath = argv[1];
    char* sequenceDirectory = argv[2];
    std::string subNetworkName = std::string(argv[3]);
    char* cpuAffinityPolicyName = argv[4];
    DeviceTypeIn device = d_CPU;
    std::vector<std::string> outputTensorNames;
    if (subNetworkName == std::string("encoder_decoder")) {
        outputTensorNames.push_back("decoder_result");
        outputTensorNames.push_back("decoder_position");
    } else if (subNetworkName == std::string("postnet")) {
        outputTensorNames.push_back("mel");
    } else if (subNetworkName == std::string("melgan_vocoder")) {
        outputTensorNames.push_back("output");
    } else {
        std::cerr << "[ERROR] unrecognized sub network(encoder_decoder|postnet) " << subNetworkName << std::endl;
        exit(1);
    }

    DataType dt;
    std::string modelPathStr = std::string(modelPath);
    //"_f[16|32].bolt"
    std::string modelPathSuffix = modelPathStr.substr(modelPathStr.size() - 9);
    if (modelPathSuffix == std::string("_f16.bolt"))
        dt = DT_F16;
    else if (modelPathSuffix == std::string("_f32.bolt"))
        dt = DT_F32;
    else if (modelPathSuffix == std::string("t8_q.bolt"))
        dt = DT_F16;
    else {
        std::cerr << "[ERROR] unrecognized model file path suffix " << modelPathSuffix << std::endl;
        exit(1);
    }
    auto pipeline = createPipeline(cpuAffinityPolicyName, modelPath, device);

    double totalTime = 0;
    int loops = 1;
    U32 falseResult = 0;
    for (int i = 0; i < loops; i++) {
        HashMap<std::string, Tensor> input = prepareStates(dt, sequenceDirectory, "input_shape.txt");
        HashMap<std::string, TensorDesc> inputDescMap;
        for (auto iter: input)
            inputDescMap[iter.first] = iter.second.get_desc();
        pipeline->reready(inputDescMap);
        for (auto iter: input) {
            U8* tensorPointer = iter.second.get_val();
            pipeline->copy_to_named_input(iter.first, tensorPointer);
        }

        double timeBegin = ut_time_ms();
        pipeline->run();
        double timeEnd = ut_time_ms();
        totalTime += (timeEnd - timeBegin);
        std::vector<Tensor> output;
        for (auto outputTensorName: outputTensorNames) {
            Tensor outputTensor = pipeline->get_tensor_by_name(outputTensorName);
            output.push_back(outputTensor);
        }
        falseResult += verify(output[0], subNetworkName);
        saveStates(pipeline, sequenceDirectory, "output_name.txt", "output_shape.txt");
    }
    UTIL_TIME_STATISTICS

    std::cout << "[SUMMARY]:" << std::endl;
    U32 validSequence = loops;
    CI_info("text to speech correct rate: " << 100.0 * (validSequence - falseResult) / validSequence  << " %");
    CI_info("avg_time:" << 1.0 * totalTime / validSequence << "ms/sequence");
    if (falseResult > 0) {
        std::cerr << "[ERROR] verify failed" << std::endl;
        exit(1);
    }

    return 0;
}
