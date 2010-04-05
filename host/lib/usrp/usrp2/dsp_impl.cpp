//
// Copyright 2010 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "usrp2_impl.hpp"
#include "usrp2_regs.hpp"
#include <uhd/usrp/dsp_props.hpp>
#include <uhd/utils/assert.hpp>
#include <boost/format.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/math/special_functions/round.hpp>

using namespace uhd;
using namespace uhd::usrp;

static const size_t default_decim = 16;
static const size_t default_interp = 16;

#define rint boost::math::iround

template <class T> T log2(T num){
    return std::log(num)/std::log(T(2));
}

/***********************************************************************
 * DDC Helper Methods
 **********************************************************************/
static boost::uint32_t calculate_freq_word_and_update_actual_freq(double &freq, double clock_freq){
    double scale_factor = std::pow(2.0, 32);

    //calculate the freq register word
    boost::uint32_t freq_word = rint((freq / clock_freq) * scale_factor);

    //update the actual frequency
    freq = (double(freq_word) / scale_factor) * clock_freq;

    return freq_word;
}

static boost::uint32_t calculate_iq_scale_word(boost::int16_t i, boost::int16_t q){
    return (boost::uint16_t(i) << 16) | (boost::uint16_t(q) << 0);
}

void usrp2_impl::init_ddc_config(void){
    //create the ddc in the rx dsp dict
    _rx_dsps["ddc0"] = wax_obj_proxy::make(
        boost::bind(&usrp2_impl::ddc_get, this, _1, _2),
        boost::bind(&usrp2_impl::ddc_set, this, _1, _2)
    );

    //initial config and update
    _ddc_decim = default_decim;
    _ddc_freq = 0;
    update_ddc_config();

    //initial command that kills streaming (in case if was left on)
    issue_ddc_stream_cmd(stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
}

void usrp2_impl::update_ddc_config(void){
    //set the decimation
    this->poke32(FR_DSP_RX_DECIM_RATE, _ddc_decim);

    //set the scaling
    static const boost::int16_t default_rx_scale_iq = 1024;
    this->poke32(FR_DSP_RX_SCALE_IQ,
        calculate_iq_scale_word(default_rx_scale_iq, default_rx_scale_iq)
    );
}

void usrp2_impl::issue_ddc_stream_cmd(const stream_cmd_t &stream_cmd){
    //setup the out data
    usrp2_ctrl_data_t out_data;
    out_data.id = htonl(USRP2_CTRL_ID_SEND_STREAM_COMMAND_FOR_ME_BRO);
    out_data.data.stream_cmd.now = (stream_cmd.stream_now)? 1 : 0;
    out_data.data.stream_cmd.secs = htonl(stream_cmd.time_spec.secs);
    out_data.data.stream_cmd.ticks = htonl(stream_cmd.time_spec.ticks);

    //set these to defaults, then change in the switch statement
    out_data.data.stream_cmd.continuous = 0;
    out_data.data.stream_cmd.chain = 0;
    out_data.data.stream_cmd.num_samps = htonl(stream_cmd.num_samps);

    //setup chain, num samps, and continuous below
    switch(stream_cmd.stream_mode){
    case stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        out_data.data.stream_cmd.continuous = 1;
        break;

    case stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS:
        out_data.data.stream_cmd.num_samps = htonl(0);
        break;

    case stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE:
        //all set by defaults above
        break;

    case stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_MORE:
        out_data.data.stream_cmd.chain = 1;
        break;
    }

    //send and recv
    usrp2_ctrl_data_t in_data = ctrl_send_and_recv(out_data);
    ASSERT_THROW(htonl(in_data.id) == USRP2_CTRL_ID_GOT_THAT_STREAM_COMMAND_DUDE);
}

/***********************************************************************
 * DDC Properties
 **********************************************************************/
void usrp2_impl::ddc_get(const wax::obj &key, wax::obj &val){
    //handle the case where the key is an expected dsp property
    if (key.type() == typeid(dsp_prop_t)){
        switch(key.as<dsp_prop_t>()){
        case DSP_PROP_NAME:
            val = std::string("usrp2 ddc0");
            return;

        case DSP_PROP_OTHERS:{
                prop_names_t others = boost::assign::list_of
                    ("if_rate")
                    ("bb_rate")
                    ("decim")
                    ("decims")
                    ("freq")
                    ("stream_cmd")
                ;
                val = others;
            }
            return;
        }
    }

    //handle string-based properties specific to this dsp
    std::string key_name = key.as<std::string>();
    if (key_name == "if_rate"){
        val = get_master_clock_freq();
        return;
    }
    else if (key_name == "bb_rate"){
        val = get_master_clock_freq()/_ddc_decim;
        return;
    }
    else if (key_name == "decim"){
        val = _ddc_decim;
        return;
    }
    else if (key_name == "decims"){
        val = _allowed_decim_and_interp_rates;
        return;
    }
    else if (key_name == "freq"){
        val = _ddc_freq;
        return;
    }

    throw std::invalid_argument(str(
        boost::format("error getting: unknown key with name %s") % key_name
    ));
}

void usrp2_impl::ddc_set(const wax::obj &key, const wax::obj &val){
    //handle string-based properties specific to this dsp
    std::string key_name = key.as<std::string>();
    if (key_name == "decim"){
        size_t new_decim = val.as<size_t>();
        assert_has(
            _allowed_decim_and_interp_rates,
            new_decim, "usrp2 decimation"
        );
        _ddc_decim = new_decim; //shadow
        update_ddc_config();
        return;
    }
    else if (key_name == "freq"){
        double new_freq = val.as<double>();
        ASSERT_THROW(new_freq <= get_master_clock_freq()/2.0);
        ASSERT_THROW(new_freq >= -get_master_clock_freq()/2.0);
        _ddc_freq = new_freq; //shadow
        this->poke32(FR_DSP_RX_FREQ,
            calculate_freq_word_and_update_actual_freq(_ddc_freq, get_master_clock_freq())
        );
        return;
    }
    else if (key_name == "stream_cmd"){
        issue_ddc_stream_cmd(val.as<stream_cmd_t>());
        return;
    }

    throw std::invalid_argument(str(
        boost::format("error setting: unknown key with name %s") % key_name
    ));
}

/***********************************************************************
 * DUC Helper Methods
 **********************************************************************/
void usrp2_impl::init_duc_config(void){
    //create the duc in the tx dsp dict
    _tx_dsps["duc0"] = wax_obj_proxy::make(
        boost::bind(&usrp2_impl::duc_get, this, _1, _2),
        boost::bind(&usrp2_impl::duc_set, this, _1, _2)
    );

    //initial config and update
    _duc_interp = default_interp;
    _duc_freq = 0;
    update_duc_config();
}

void usrp2_impl::update_duc_config(void){
    // Calculate CIC interpolation (i.e., without halfband interpolators)
    size_t tmp_interp = _duc_interp;
    while(tmp_interp > 128) tmp_interp /= 2;

    // Calculate closest multiplier constant to reverse gain absent scale multipliers
    double interp_cubed = std::pow(double(tmp_interp), 3);
    boost::int16_t scale = rint((4096*std::pow(2, ceil(log2(interp_cubed))))/(1.65*interp_cubed));

    //set the interpolation
    this->poke32(FR_DSP_TX_INTERP_RATE, _ddc_decim);

    //set the scaling
    this->poke32(FR_DSP_TX_SCALE_IQ, calculate_iq_scale_word(scale, scale));
}

/***********************************************************************
 * DUC Properties
 **********************************************************************/
void usrp2_impl::duc_get(const wax::obj &key, wax::obj &val){
    //handle the case where the key is an expected dsp property
    if (key.type() == typeid(dsp_prop_t)){
        switch(key.as<dsp_prop_t>()){
        case DSP_PROP_NAME:
            val = std::string("usrp2 duc0");
            return;

        case DSP_PROP_OTHERS:{
                prop_names_t others = boost::assign::list_of
                    ("if_rate")
                    ("bb_rate")
                    ("interp")
                    ("interps")
                    ("freq")
                ;
                val = others;
            }
            return;
        }
    }

    //handle string-based properties specific to this dsp
    std::string key_name = key.as<std::string>();
    if (key_name == "if_rate"){
        val = get_master_clock_freq();
        return;
    }
    else if (key_name == "bb_rate"){
        val = get_master_clock_freq()/_duc_interp;
        return;
    }
    else if (key_name == "interp"){
        val = _duc_interp;
        return;
    }
    else if (key_name == "interps"){
        val = _allowed_decim_and_interp_rates;
        return;
    }
    else if (key_name == "freq"){
        val = _duc_freq;
        return;
    }

    throw std::invalid_argument(str(
        boost::format("error getting: unknown key with name %s") % key_name
    ));
}

void usrp2_impl::duc_set(const wax::obj &key, const wax::obj &val){
    //handle string-based properties specific to this dsp
    std::string key_name = key.as<std::string>();
    if (key_name == "interp"){
        size_t new_interp = val.as<size_t>();
        assert_has(
            _allowed_decim_and_interp_rates,
            new_interp, "usrp2 interpolation"
        );
        _duc_interp = new_interp; //shadow
        update_duc_config();
        return;
    }
    else if (key_name == "freq"){
        double new_freq = val.as<double>();
        ASSERT_THROW(new_freq <= get_master_clock_freq()/2.0);
        ASSERT_THROW(new_freq >= -get_master_clock_freq()/2.0);
        _duc_freq = new_freq; //shadow
        this->poke32(FR_DSP_TX_FREQ,
            calculate_freq_word_and_update_actual_freq(_duc_freq, get_master_clock_freq())
        );
        return;
    }

    throw std::invalid_argument(str(
        boost::format("error setting: unknown key with name %s") % key_name
    ));
}
