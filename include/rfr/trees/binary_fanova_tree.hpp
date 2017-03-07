#ifndef RFR_FANOVA_TREE_HPP
#define RFR_FANOVA_TREE_HPP

#include <vector>
#include <deque>
#include <stack>
#include <utility>       // std::pair
#include <algorithm>     // std::shuffle
#include <numeric>       // std::iota
#include <cmath>         // std::abs
#include <iterator>      // std::advance
#include <fstream>
#include <random>


#include "cereal/cereal.hpp"
#include <cereal/types/bitset.hpp>
#include <cereal/types/vector.hpp>

#include "rfr/data_containers/data_container.hpp"
#include "rfr/nodes/temporary_node.hpp"
#include "rfr/nodes/k_ary_node.hpp"
#include "rfr/trees/tree_base.hpp"
#include "rfr/trees/tree_options.hpp"
#include "rfr/util.hpp"

#include "rfr/trees/k_ary_tree.hpp"
#include "rfr/splits/binary_split_one_feature_rss_loss.hpp"


namespace rfr{ namespace trees{

template <typename split_t, typename num_t = float, typename response_t = float, typename index_t = unsigned int, typename rng_t = std::default_random_engine>
class binary_fANOVA_tree : public k_ary_random_tree<2,  rfr::nodes::k_ary_node_full<2, split_t, num_t, response_t, index_t, rng_t> , num_t, response_t, index_t, rng_t> {

  private:
	typedef rfr::trees::k_ary_random_tree<2, rfr::nodes::k_ary_node_full<2, split_t, num_t, response_t, index_t, rng_t>, num_t, response_t, index_t, rng_t> super;
  protected:


	std::vector<num_t> subspace_sizes;					// size of the subspace in node's subtree
	std::vector<num_t> marginal_prediction;				// prediction of the subtree below a node
	std::vector<std::vector<bool>  > active_variables;	// note: vector<bool> uses bitwise operations, so it might be too slow

	
	std::vector<std::vector<num_t> > split_values;
	
  public:
  
	binary_fANOVA_tree(): super(), split_values(0) {}

	virtual ~binary_fANOVA_tree() {}
	
    /* serialize function for saving forests */
  	template<class Archive>
  	void serialize(Archive & archive){
		super::serialize();
		
	}


	/* \brief fit the fANOVA forest
	 *
	 * Overloads the ancestor's method to reinitialize variables after fitting.
	 */
	virtual void fit(const rfr::data_containers::base<num_t, response_t, index_t> &data,
			 rfr::trees::tree_options<num_t, response_t, index_t> tree_opts,
			 const std::vector<num_t> &sample_weights,
			 rng_t &rng){
				 
			super::fit(data, tree_opts, sample_weights, rng);
	}

	/* \brief function to recursively compute the marginalized predictions
	 * 
	 * To compute the fANOVA, the mean prediction over partial assingments is needed.
	 * To accomplish that, feed this function a numerical vector where each element that
	 * is NAN will be marginalized over.
     * 
	 * At any split, this function either follows one path or averages the
	 * prediction of all children weighted by the subspace size. If the subtree
	 * does not split on any of the 'active' features, a pre-computed values is used.
     * 
     * \param feature_vector the features vector with NAN for dimensions over which is marginalized
     * 
     * \returns the mean prediction marginalized over the desired inputs, NAN if the cutoffs exclude all potential leaves the feature vector would fall in
	 * */

	num_t marginalized_mean_prediction(const std::vector<num_t> &feature_vector) const{

		auto active_features = rfr::util::get_non_NAN_indices(feature_vector);
		std::deque<index_t> active_nodes;
		active_nodes.push_back(0);

		// to average the predictions of the individual leafes/nodes
		rfr::util::weighted_running_statistics<num_t> stats;

		while (active_nodes.size() > 0){
			
			index_t i = active_nodes.back();
			active_nodes.pop_back();
			
			// three cases
			// 1. active node has subspace size zero -> no weight here -> skip
			if (subspace_sizes[i] == 0)	continue;
			

			// 2. node's subtree split on any active varialble and 
			if (rfr::util::any_true(active_variables[i], active_features)){
			//		2.a. node itself splits on an active variable -> add corresponding child to active nodes
			//		2.b. note itself does NOT split on an active variable -> add both children to active nodes
			}
			// 3. node's subtree does not split on any of the active variables -> add to statistics
			else{
				stats.push( marginal_prediction[i], subspace_sizes[i]);
			}
		}
		return stats.mean();
	}
	/* \brief precomputes the marginal prediction in each node based on the subspace sizes
	 *
	 * To compute the fANOVA faster, the tree can efficiently compute and cache the marginal
	 * prediction for the subtree of any node. Combined with storing which variables remain constant
	 * within it, this should reduce the computational overhead; at least for not too important variables. */
	void precompute_marginals(num_t lower_cutoff, num_t upper_cutoff,
		const std::vector<std::vector<num_t> >& pcs, const std::vector<index_t>& types){

		/* This function should work in two steps:
		 * 		1.	Compute the size of the subspace for each node. See partition_recursor
		 * 			for an example. This is can be done in a top-down fashion.
		 * 		2. 	Starting from the leaves, the marginalized prediction can be computed recursively
		 * 			by averaging the prediction from the children w.r.t their subspace size.
		 * 			Note, the cutoffs should be used to exclude leaves with a prediction outside the
		 * 			bounds.
		 * 			During this step the active variables should also be stored, such that it can
		 * 			be checked if the subtree's prediction depends on any of the 'active' variables
		 */
		assert(pcs.size() == types.size());
		size_t num_features = types.size();

		subspace_sizes.resize(super::the_nodes.size());
		active_variables.resize(super::the_nodes.size());
		marginal_prediction.resize(super::the_nodes.size());

		// unfortunately, the subspaces have to be stored on the downward pass
		// this could be done dynamically by only storing the elements still needed, but for 
		// simplicity, let's store all of them for now
		std::vector< std::vector< std::vector <num_t> > > subspaces(super::the_nodes.size());
		subspaces[0] = pcs;


		// down pass
		for (index_t i = 0; i < super::the_nodes.size(); ++i) {
			auto & n = super::the_nodes[i];
			subspace_sizes[i] = rfr::util::subspace_cardinality(subspaces, types);
			
			auto subss = n.compute_subspaces(pcs);
			subspaces[n.get_child_index(0)] = subss[0];
			subspaces[n.get_child_index(1)] = subss[1];
			
			// initialize the active variables
			active_variables[i] = std::vector<bool>(0,num_features);
			
			// delete no longer needed subspaces right away
			subspaces[i].clear();
		}


		for (index_t node_index = super::the_nodes.size() - 1; node_index >= 0; --i) {
			if (super::the_nodes[node_index].is_a_leaf()) {
				// TODO: add lower and upper cutoff checks here
				marginal_prediction[node_index] = super::the_nodes[node_index].leaf_statistic().mean(); // Get leaf's mean prediction
			}
			else{
				// record active variable
				active_variables[node_index][super::the_nodes[node_index].get_split().get_feature_index()] = true;
				
				// propagate to parent
				index_t parent_index = super::the_nodes[node_index].parent();
				rfr::util::disjunction(active_variables[node_index], active_variables[parent_index]);

				// compute marginal prediction
				marginal_prediction[node_index] = 0.0;
				num_t children_subspace_size = 0;
				
				for (index_t child_index : super::the_nodes[node_index].get_children()) {
					if (subspace_size[child_index] > 0.0){
						children_subspace_sizes += subspace_sizes[child_index];
						marginal_prediction[node_index] += marginal_prediction[child_index] * subspace_sizes[child_index];
					}
				}
				if (children_subspace_size > 0)
					marginal_prediction[node_index] /= children_subspace_size;
				else
					marginal_prediction[node_index] = NaN;
				
				// the subspace size is updated to account for the cutoffs which might 'deactivate' certain leaves.
				// this will be reflected by a zero subspace size for a node!
				subspace_sizes[node_index] = children_subspace_size;
			}
		}
	}

  num_t get_subspace_size(index_t node_index) {
    return subspace_sizes[node_index];
  }
  
  const std::vector<bool>& get_vars(index_t node_index) {
    return active_variables[node_index];
  }

  const std::vector<rfr::nodes::k_ary_node_full<2, split_t, num_t, response_t, index_t, rng_t>>& get_nodes() {
    return super::the_nodes;
  }


	////////////////////////////////////////////////////////////////////
	// LEGACY CODE below, should be refactored/removed soon!
	////////////////////////////////////////////////////////////////////

	/* \brief finds all the split points for each dimension of the input space
	 * 
	 * This function only makes sense for axis aligned splits!
	 * */

	 
	std::vector<std::vector<num_t> > all_split_values (const std::vector<index_t> &types) {
		
		if (split_values.size() == 0){
			
			split_values.resize(types.size());
			
			for (auto &n: super::the_nodes){
				if (n.is_a_leaf()) continue;
				
				const auto &s = n.get_split();
				auto fi = s.get_feature_index();

				// if a split on a categorical occurs, just add all its possible values
				if((types[fi] > 0) && (split_values[fi].size() == 0)){
					split_values[fi].resize(types[fi]);
					std::iota(split_values[fi].begin(), split_values[fi].end(), 0);
				}
				else{
					split_values[fi].emplace_back(s.get_num_split_value());
				}
			}
			
			for (auto &v: split_values)
				std::sort(v.begin(), v.end());
		}
		return(split_values);
	}

	
	/* \brief Function to recursively compute the partition induced by the tree
	 *
	 * Do not call this function from the outside! Needs become private at some point!
	 */
	 /*
	void partition_recursor (	std::vector<std::vector< std::vector<num_t> > > &the_partition,
							std::vector<std::vector<num_t> > &subspace, num_t node_index) const {

		// add subspace for a leaf
		if (the_nodes[node_index].is_a_leaf())
			the_partition.push_back(subspace);
		else{
			// compute subspaces of children
			auto subs = the_nodes[node_index].compute_subspaces(subspace);
			// recursively go trough the tree
			for (auto i=0u; i<k; i++){
				partition_recursor(the_partition, subs[i], the_nodes[node_index].get_child_index(i));
			}
		}
	}
	*/


	/* \brief computes the partitioning of the input space induced by the tree */
	/*
	std::vector<std::vector< std::vector<num_t> > > partition( std::vector<std::vector<num_t> > pcs) const {
	
		std::vector<std::vector< std::vector<num_t> > > the_partition;
		the_partition.reserve(num_leafs);
		
		partition_recursor(the_partition, pcs, 0);
	
	return(the_partition);
	}

	*/


};

}}//namespace rfr::trees
#endif
