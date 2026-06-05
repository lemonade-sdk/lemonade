/// \file modeling_gemma_embedding.hpp
/// \brief Gemma_Embedding class
/// \author FastFlowLM Team
/// \date 2025-10-23
/// \version 0.9.24
/// \note This is a header file for the Gemma_Embedding class

#include "auto_embedding_model.hpp"
#include "models/gemma_embedding/gemma_embedding.hpp"

class Gemma_Embedding : public AutoEmbeddingModel{
private:
    std::string _get_task_prefix(embedding_task_type_t task_type){
        /*
        "query": "task: search result | query: ",
        "document": "title: none | text: ",
        "BitextMining": "task: search result | query: ",
        "Clustering": "task: clustering | query: ",
        "Classification": "task: classification | query: ",
        "InstructionRetrieval": "task: code retrieval | query: ",
        "MultilabelClassification": "task: classification | query: ",
        "PairClassification": "task: sentence similarity | query: ",
        "Reranking": "task: search result | query: ",
        "Retrieval": "task: search result | query: ",
        "Retrieval-query": "task: search result | query: ",
        "Retrieval-document": "title: none | text: ",
        "STS": "task: sentence similarity | query: ",
        "Summarization": "task: summarization | query: "
        */
        if (task_type == task_query) {
            return "task: search result | query: ";
        } else if (task_type == task_document) {
            return "title: none | text: ";
        } else if (task_type == task_bitextmining) {
            return "task: search result | query: ";
        } else if (task_type == task_clustering) {
            return "task: clustering | query: ";
        } else if (task_type == task_classification) {
            return "task: classification | query: ";
        } else if (task_type == task_code_retrieval) {
            return "task: code retrieval | query: ";
        } else if (task_type == task_multilabel_classification) {
            return "task: classification | query: ";
        } else if (task_type == task_sentence_similarity) {
            return "task: sentence similarity | query: ";
        } else if (task_type == task_search_result) {
            return "task: search result | query: ";
        } else if (task_type == task_summarization) {
            return "task: summarization | query: ";
        } else {
            return "";
        }
    }
public:
    Gemma_Embedding(xrt::device* npu_device_inst);
    ~Gemma_Embedding();

    void load_model(std::string model_path, json model_info, bool enable_preemption = false) override;
    std::vector<float> embed(std::string& text, embedding_task_type_t task_type) override;
};
