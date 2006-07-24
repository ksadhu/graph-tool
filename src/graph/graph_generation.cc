// graph-tool -- a general graph modification and manipulation thingy
//
// Copyright (C) 2006  Tiago de Paula Peixoto <tiago@forked.de>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#include <algorithm>
#include <tr1/unordered_set>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/random.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <iomanip>
#include <map>

#include "graph.hh"
#include "histogram.hh"

using namespace std;
using namespace boost;
using namespace boost::lambda;
using namespace multi_index;
using namespace graph_tool;

typedef boost::mt19937 rng_t;

//==============================================================================
// sample_from_distribution
// this will sample a (j,k) pair from a pjk distribution given a ceil function
// and its inverse
//==============================================================================

template <class Distribution, class Ceil, class InvCeil>
struct sample_from_distribution
{
    sample_from_distribution(Distribution &dist, Ceil& ceil, InvCeil &inv_ceil, double bound, rng_t& rng)
	: _dist(dist), _ceil(ceil), _inv_ceil(inv_ceil), _bound(bound), _rng(rng), _uniform_p(0.0, 1.0) {}
    
    pair<size_t, size_t> operator()()
    {
	// sample j,k from ceil
	size_t j,k;
	double u;
	do
	{
	    tie(j,k) = _inv_ceil(_uniform_p(_rng), _uniform_p(_rng));
	    u = _uniform_p(_rng);
	}
	while (u > _dist(j,k)/(_bound*_ceil(j,k)));
	return make_pair(j,k);
    }

    Distribution& _dist;
    Ceil& _ceil;
    InvCeil& _inv_ceil;
    double _bound;
    rng_t &_rng;
    boost::uniform_real<double> _uniform_p;
};

// vertex type, with desired j,k values and the index in the real graph

struct vertex_t 
{
    vertex_t() {}
    vertex_t(size_t in, size_t out): in_degree(in), out_degree(out) {}
    vertex_t(const pair<size_t,size_t>& deg): in_degree(deg.first), out_degree(deg.second) {}
    size_t index, in_degree, out_degree;
    bool operator==(const vertex_t& other) const {return other.index == index;}
};

inline std::size_t hash_value(const vertex_t& v)
{
    size_t h = hash_value(v.in_degree);
    hash_combine(h, v.out_degree);
    return h;
}

inline size_t dist(const vertex_t& a, const vertex_t& b)
{
    return int(a.in_degree-b.in_degree)*int(a.in_degree-b.in_degree) + 
	int(a.out_degree-b.out_degree)*int(a.out_degree-b.out_degree);
}

struct total_deg_comp
{
    bool operator()(const pair<size_t,size_t>& a, const pair<size_t,size_t>& b)
    {
	return a.first + a.second < b.first + b.second;
    }
};

//==============================================================================
// degree_matrix_t
// this structure will keep the existing (j,k) pairs in the graph in a matrix,
// so that the nearest (j,k) to a given target can be found easily.
//==============================================================================

class degree_matrix_t: public vector<vector<vector<pair<size_t,size_t> > > >
{
public:
    typedef vector<vector<vector<pair<size_t,size_t> > > > base_t;
    degree_matrix_t(size_t N, size_t max_j, size_t max_k, size_t avg_deg)
    {
	_L = size_t(sqrt(N));
	base_t &base = *this;
	base = base_t(_L, vector<vector<pair<size_t,size_t> > >(_L));
	_cj = pow(max_j+1,1.0/(_L-1)) - 1.0;
	_ck = pow(max_k+1,1.0/(_L-1)) - 1.0;
	_avg_deg = avg_deg;
	_nearest_bins = base_t(_L, vector<vector<pair<size_t,size_t> > >(_L));
	_size = 0;
    }
    
    void insert(const pair<size_t, size_t>& v)
    {
	size_t j_bin, k_bin;
	tie(j_bin, k_bin) = get_bin(v.first, v.second);
	if ((*this)[j_bin][k_bin].empty())
	    _size++;
	(*this)[j_bin][k_bin].push_back(v);

    }

    void arrange_proximity()
    {
	for(size_t j = 0; j < _L; ++j)
	    for(size_t k = 0; k < _L; ++k)
	    {
		_nearest_bins[j][k].clear();
		if ((*this)[j][k].empty())
		{
		    for(size_t w = 1; w < _L; ++w)
		    {
			for (size_t i = ((j>w)?j-w:0); i < ((j+w<=_L)?j+w:_L); ++i)
			    for (size_t l = ((k>w)?k-w:0); l < ((k+w<=_L)?k+w:_L); ++l)
			    {
				if (!(*this)[i][l].empty())
				    _nearest_bins[j][k].push_back(make_pair(i,l));
			    }
			if (!_nearest_bins[j][k].empty())
			    break;
		    }
		}
	    }
    }


    void erase(const pair<size_t,size_t>& v)
    {
	size_t j_bin, k_bin;
	tie(j_bin, k_bin) = get_bin(v.first, v.second);
	for(size_t i = 0; i < (*this)[j_bin][k_bin].size(); ++i)
	{
	    if ((*this)[j_bin][k_bin][i] == v)
	    {
		(*this)[j_bin][k_bin].erase((*this)[j_bin][k_bin].begin()+i);
		break;
	    }
	}
	if ((*this)[j_bin][k_bin].empty())
	{
	    _size--;
	    if (_size > _L)
		arrange_proximity();
	}
	
    }

    pair<size_t,size_t> find_closest(size_t j, size_t k, rng_t& rng)
    {
	vector<pair<size_t,size_t> > candidates;
	size_t j_bin, k_bin;
	tie(j_bin, k_bin) = get_bin(j, k);

	if ((*this)[j_bin][k_bin].empty())
	{
	    if (_size > _L)
	    {
		if (_nearest_bins[j_bin][k_bin].empty())
		    arrange_proximity();
		for(size_t i = 0; i < _nearest_bins[j_bin][k_bin].size(); ++i)
		{
		    size_t jb,kb;
		    tie(jb,kb) = _nearest_bins[j_bin][k_bin][i];
		    search_bin(jb, kb, j, k, candidates);
		}
	    }
	    else
	    {
		for(size_t jb = 0; jb < _L; ++jb)
		    for(size_t kb = 0; kb < _L; ++kb)
			search_bin(jb, kb, j, k, candidates);
	    }
	}
	else
	{	    
	    search_bin(j_bin, k_bin, j, k, candidates);

	    size_t distance = size_t(sqrt(dist(candidates.front(), vertex_t(j,k))));
	    
	    for (int x = -1; x < 2; ++x)
		for (int y = -1; y < 2; ++y)
		{ 
		    size_t jb,kb;
		    tie(jb,kb) = get_bin(max(int(j+x*distance),0), max(int(k+y*distance),0));
		    if (tie(jb,kb) == tie(j_bin,k_bin))
			continue;
		    search_bin(jb, kb, j, k, candidates);
		}
	}
	
	uniform_int<size_t> sample(0, candidates.size() - 1);
	return candidates[sample(rng)];
    }

private:
    pair<size_t,size_t> get_bin(size_t j, size_t k) 
    {
	// uses logarithmic binning...
	size_t j_bin, k_bin, lim;
	lim = size_t(1.5*_avg_deg);
	if (j < lim)
	    j_bin = j;
	else
	    j_bin = size_t(log(j)/log(_cj+1)) - size_t(log(lim)/log(_cj+1)) + lim;
	if (k < lim)
	    k_bin = k;
	else
	    k_bin = size_t(log(k)/log(_ck+1)) - size_t(log(lim)/log(_cj+1)) + lim;
	return make_pair(min(j_bin, _L-1), min(k_bin,_L-1));
    }
    
    void search_bin(size_t j_bin, size_t k_bin, size_t j, size_t k, vector<pair<size_t,size_t> >& candidates)
    {
	for (typeof((*this)[j_bin][k_bin].begin()) iter = (*this)[j_bin][k_bin].begin(); iter != (*this)[j_bin][k_bin].end(); ++iter)
	{
	    if (candidates.empty())
	    {
		candidates.push_back(*iter);
		continue;
	    }
	    if (dist(vertex_t(*iter), vertex_t(j,k)) < dist(vertex_t(candidates.front()),vertex_t(j,k)))
	    {
		candidates.clear();
		candidates.push_back(*iter);
	    }
	    else if (dist(vertex_t(*iter), vertex_t(j,k)) == dist(vertex_t(candidates.front()),vertex_t(j,k)))
	    {
		candidates.push_back(*iter);
	    }
	}
    }

    size_t _L;
    double _cj;
    double _ck;
    size_t _avg_deg;
    base_t _nearest_bins;
    size_t _size;
};

//==============================================================================
// GenerateCorrelatedConfigurationalModel
// generates a directed graph with given pjk and degree correlation
//==============================================================================
void GraphInterface::GenerateCorrelatedConfigurationalModel(size_t N, pjk_t pjk, pjk_t ceil_pjk, inv_ceil_t inv_ceil_pjk, double ceil_pjk_bound,
							    corr_t corr, corr_t ceil_corr, inv_corr_t inv_ceil_corr, double ceil_corr_bound, 
							    bool undirected_corr, size_t seed, bool verbose)
{
    _mg.clear();
    _properties = dynamic_properties();
    rng_t rng(seed);

    // sample the N (j,k) pairs

    sample_from_distribution<pjk_t, pjk_t, inv_ceil_t> pjk_sample(pjk, ceil_pjk, inv_ceil_pjk, ceil_pjk_bound, rng);
    vector<vertex_t> vertices(N);
    size_t sum_j=0, sum_k=0, max_j=0, max_k=0;
    if (verbose)
    {
	cout << "adding vertices: " << flush;
    }
    for(size_t i = 0; i < N; ++i)
    {
	if (verbose)
	{
	    static stringstream str;
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << "\b";
	    str.str("");
	    str << i+1 << " of " << N << " (" << (i+1)*100/N << "%)";
	    cout << str.str() << flush;
	}
	vertex_t& v = vertices[i];
	v.index = _vertex_index[add_vertex(_mg)];
	tie(v.in_degree, v.out_degree) = pjk_sample();
	sum_j += v.in_degree;
	sum_k += v.out_degree;
	max_j = max(v.in_degree,max_j);
	max_k = max(v.out_degree,max_k); 
    }

    if (verbose)
	cout << "\nfixing average degrees: " << flush;

    // <j> and <k> must be the same. Resample random pairs until this holds.
    uniform_int<size_t> vertex_sample(0, N-1);
    while (sum_j != sum_k)
    {
	vertex_t& v = vertices[vertex_sample(rng)];
	sum_j -= v.in_degree;
	sum_k -= v.out_degree;
	tie(v.in_degree, v.out_degree) = pjk_sample();
	sum_j += v.in_degree;
        sum_k +=  v.out_degree;
	max_j = max(v.in_degree,max_j);
	max_k = max(v.out_degree,max_k);
	if (verbose)
	{
	    static stringstream str;
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << "\b";
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << " ";
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << "\b";
	    str.str("");
	    str << min(sum_j-sum_k, sum_k-sum_j);
	    cout << str.str() << flush;
	}
    }

    size_t E = sum_k;
 
    vector<vertex_t> sources; // sources of edges
    typedef tr1::unordered_multimap<pair<size_t,size_t>, vertex_t, hash<pair<size_t,size_t> > > targets_t;
    targets_t targets; // vertices with j > 0
    typedef tr1::unordered_set<pair<size_t,size_t>, hash<pair<size_t,size_t> > > target_degrees_t;
    target_degrees_t target_degrees; // existing (j,k) pairs
    
    // fill up sources, targets and target_degrees
    sources.reserve(E);
    for(size_t i = 0; i < N; ++i)
    {
	for(size_t k = 0; k < vertices[i].out_degree; ++k)
	    sources.push_back(vertices[i]);
	if (vertices[i].in_degree > 0)
	{
	    targets.insert(make_pair(make_pair(vertices[i].in_degree, vertices[i].out_degree), vertices[i]));
	    target_degrees.insert(make_pair(vertices[i].in_degree, vertices[i].out_degree));
	}
    }

    typedef multiset<pair<size_t,size_t>, total_deg_comp> ordered_degrees_t;
    ordered_degrees_t ordered_degrees; // (j,k) pairs ordered by (j+k), i.e, total degree
    degree_matrix_t degree_matrix(target_degrees.size(), max_j, max_k, E/N); // (j,k) pairs layed out in a 2 dimensional matrix
    for(typeof(target_degrees.begin()) iter = target_degrees.begin(); iter != target_degrees.end(); ++iter)
	if (undirected_corr)
	    ordered_degrees.insert(*iter);
	else
	    degree_matrix.insert(*iter);
    
    // shuffle sources 
    for (size_t i = 0; i < sources.size(); ++i)
    {
	uniform_int<size_t> source_sample(i, sources.size()-1);
	swap(sources[i], sources[source_sample(rng)]);
    }

    if (verbose)
	cout << "\nadding edges: " << flush;

    // connect the sources to targets
    uniform_real<double> sample_probability(0.0, 1.0); 
    for (size_t i = 0; i < sources.size(); ++i)
    {
	vertex_t source = sources[i], target;
	size_t j = source.in_degree;
	size_t k = source.out_degree;
	
	//choose the target vertex according to correlation
	    
	pjk_t prob_func = lambda::bind(corr,lambda::_1,lambda::_2,j,k);
	pjk_t ceil = lambda::bind(ceil_corr,lambda::_1,lambda::_2,j,k);
	inv_ceil_t inv_ceil = lambda::bind(inv_ceil_corr,lambda::_1,lambda::_2,j,k);
	sample_from_distribution<pjk_t, pjk_t, inv_ceil_t> corr_sample(prob_func, ceil, inv_ceil, ceil_corr_bound, rng);
	
	size_t jl,kl;
	tie(jl,kl) = corr_sample(); // target (j,k)
	
	target_degrees_t::iterator iter = target_degrees.find(make_pair(jl,kl));
	if (iter != target_degrees.end())
	{
	    target = targets.find(*iter)->second; // if an (jl,kl) pair exists, just use that
	}
	else
	{	
	    pair<size_t, size_t> deg;
	    if (undirected_corr)
	    {
		// select the (j,k) pair with the closest total degree (j+k)
		ordered_degrees_t::iterator upper;
		upper = ordered_degrees.upper_bound(make_pair(jl,kl));
		if (upper == ordered_degrees.end())
		{
		    --upper;
		    deg = *upper;
		}
		else if (upper == ordered_degrees.begin())
		{
		    deg = *upper;
		}
		else
		{
		    ordered_degrees_t::iterator lower = upper;
		    --lower;
		    if (jl + kl - (lower->first + lower->second) < upper->first + upper->second - (jl + kl))
			deg = *lower;
		    else if (jl + kl - (lower->first + lower->second) != upper->first + upper->second - (jl + kl))
			deg = *upper;
		    else
		    {
			// if equal, choose randomly with equal probability
			uniform_int<size_t> sample(0, 1);
			if (sample(rng))
			    deg = *lower;
			else
			    deg = *upper;
		    }
		}
		target = targets.find(deg)->second;
	    }
	    else
	    {   
		// select the (j,k) which is the closest in the j,k plane.
		deg = degree_matrix.find_closest(jl, kl, rng);
		target = targets.find(deg)->second;
//		cerr << "wanted: " << jl << ", " << kl
//		     << " got: " << deg.first << ", " << deg.second << "\n";
	       
	    }	    
	}

	//add edge
	graph_traits<multigraph_t>::edge_descriptor e;
	e = add_edge(vertex(source.index, _mg), vertex(target.index, _mg), _mg).first;
	_edge_index[e] = i;

	// if target received all the edges it should, remove it from target
	if (in_degree(vertex(target.index, _mg), _mg) == target.in_degree)
	{
	    targets_t::iterator iter,end;
	    for(tie(iter,end) = targets.equal_range(make_pair(target.in_degree, target.out_degree)); iter != end; ++iter)
		if (iter->second == target)
		{
		    targets.erase(iter);
		    break;
		}

	    // if there are no more targets with (jl,kl), remove pair from target_degrees, etc.
	    if (targets.find(make_pair(target.in_degree, target.out_degree)) == targets.end())
	    {
		target_degrees.erase(target_degrees.find(make_pair(target.in_degree, target.out_degree)));
		if (target_degrees.bucket_count() > 2*target_degrees.size())
		{
		    target_degrees_t temp;
		    for(target_degrees_t::iterator iter = target_degrees.begin(); iter != target_degrees.end(); ++iter)
			temp.insert(*iter);
		    target_degrees = temp;
		}
		if (undirected_corr)
		{
		    for(ordered_degrees_t::iterator iter = ordered_degrees.find(make_pair(target.in_degree, target.out_degree)); 
			iter != ordered_degrees.end(); ++iter)
			if (*iter == make_pair(target.in_degree, target.out_degree))
			{
			    ordered_degrees.erase(iter);
			    break;
			}
		}
		else
		{
		    degree_matrix.erase(make_pair(target.in_degree, target.out_degree));
		}
	    }
	    
	}

	if (verbose)
	{
	    static stringstream str;	    
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << "\b";
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << " ";
	    for (size_t j = 0; j < str.str().length(); ++j)
		cout << "\b";
	    str.str("");
	    str << (i+1) << " of " << E << " (" << (i+1)*100/E << "%)";
	    cout << str.str() << flush;
	}
	
    }
    
    if (verbose)
	cout << "\n";
}