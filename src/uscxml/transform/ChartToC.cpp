/**
 *  @file
 *  @author     2012-2015 Stefan Radomski (stefan.radomski@cs.tu-darmstadt.de)
 *  @copyright  Simplified BSD
 *
 *  @cond
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the FreeBSD license as published by the FreeBSD
 *  project.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  You should have received a copy of the FreeBSD license along with this
 *  program. If not, see <http://www.opensource.org/licenses/bsd-license>.
 *  @endcond
 */

#include "uscxml/transform/ChartToFSM.h"
#include "uscxml/transform/ChartToC.h"
#include "uscxml/debug/Complexity.h"
#include <DOM/io/Stream.hpp>
#include <iostream>
#include "uscxml/UUID.h"
#include "uscxml/util/MD5.hpp"
#include "uscxml/DOMUtils.h"
#include <math.h>
#include <boost/algorithm/string.hpp>
#include <glog/logging.h>

#include <algorithm>
#include <iomanip>

namespace uscxml {

using namespace Arabica::DOM;
using namespace Arabica::XPath;

// many more tricks: https://graphics.stanford.edu/~seander/bithacks.html

Transformer ChartToC::transform(const Interpreter& other) {
	ChartToC* c2c = new ChartToC(other);

	return boost::shared_ptr<TransformerImpl>(c2c);
}

ChartToC::ChartToC(const Interpreter& other) : TransformerImpl(), _topMostMachine(NULL), _parentMachine(NULL) {
	cloneFrom(other.getImpl());
	std::stringstream ss;
	ss << _document;
	_md5 = md5(ss.str());
	_prefix = "_scxml_" + _md5.substr(0, 8);
	_allMachines.push_back(this);

	prepare();
	findNestedMachines();

}

void ChartToC::setHistoryCompletion() {
	std::set<std::string> elements;
	elements.insert(_nsInfo.xmlNSPrefix + "history");
	Arabica::XPath::NodeSet<std::string> histories = DOMUtils::inPostFixOrder(elements, _scxml);

	NodeSet<std::string> covered;
	NodeSet<std::string> perParentcovered;
	Node<std::string> parent;

	for (size_t i = 0; i < histories.size(); i++) {
		Element<std::string> history(histories[i]);
		NodeSet<std::string> completion;

		if (parent != history.getParentNode()) {
			covered.push_back(perParentcovered);
			perParentcovered = NodeSet<std::string>();
			parent = history.getParentNode();
		}

		bool deep = (HAS_ATTR(history, "type") && iequals(ATTR(history, "type"), "deep"));
		for (size_t j = 0; j < _states.size(); j++) {
			if (_states[j] == history)
				continue;

			if (isDescendant(_states[j], history.getParentNode()) && isHistory(Element<std::string>(_states[j]))) {
				history.setAttribute("hasHistoryChild", "yes");
			}

			if (isMember(_states[j], covered))
				continue;

			if (deep) {
				if (isDescendant(_states[j], history.getParentNode()) && !isHistory(Element<std::string>(_states[j]))) {
					completion.push_back(_states[j]);
				}
			} else {
				if (_states[j].getParentNode() == history.getParentNode() && !isHistory(Element<std::string>(_states[j]))) {
					completion.push_back(_states[j]);
				}
			}
		}
		perParentcovered.push_back(completion);

		std::string completionBools;
		for (size_t j = 0; j < _states.size(); j++) {
			if (isMember(_states[j], completion)) {
				completionBools += "1";
			} else {
				completionBools += "0";
			}
		}
		history.setAttribute("completionBools", completionBools);
	}
}

void ChartToC::resortStates(Arabica::DOM::Node<std::string>& node) {
	if (node.getNodeType() != Node_base::ELEMENT_NODE)
		return;

	/**
	  initials
	  deep histories
	  shallow histories
	  everything else
	 */

	Element<std::string> element(node);

	// shallow history states to top
	Node<std::string> child = element.getFirstChild();
	while(child) {
		resortStates(child);
		if (child.getNodeType() == Node_base::ELEMENT_NODE &&
		        TAGNAME_CAST(child) == _nsInfo.xmlNSPrefix + "history" &&
		        (!HAS_ATTR(element, "type") || iequals(ATTR(element, "type"), "shallow"))) {
			Node<std::string> tmp = child.getNextSibling();
			if (child != element.getFirstChild()) {
				element.insertBefore(child, element.getFirstChild());
			}
			child = tmp;
		} else {
			child = child.getNextSibling();
		}
	}

	// deep history states to top
	child = element.getFirstChild();
	while(child) {
		resortStates(child);
		if (child.getNodeType() == Node_base::ELEMENT_NODE &&
		        TAGNAME_CAST(child) == _nsInfo.xmlNSPrefix + "history" &&
		        HAS_ATTR(element, "type") &&
		        iequals(ATTR(element, "type"), "deep")) {

			Node<std::string> tmp = child.getNextSibling();
			if (child != element.getFirstChild()) {
				element.insertBefore(child, element.getFirstChild());
			}
			child = tmp;
		} else {
			child = child.getNextSibling();
		}
	}

	// initial states on top of histories even
	child = element.getFirstChild();
	while(child) {
		resortStates(child);
		if (child.getNodeType() == Node_base::ELEMENT_NODE && TAGNAME_CAST(child) == _nsInfo.xmlNSPrefix + "initial") {
			Node<std::string> tmp = child.getNextSibling();
			if (child != element.getFirstChild()) {
				element.insertBefore(child, element.getFirstChild());
			}
			child = tmp;
		} else {
			child = child.getNextSibling();
		}
	}

}

void ChartToC::setStateCompletion() {
	setHistoryCompletion();

	for (size_t i = 0; i < _states.size(); i++) {
		Element<std::string> state(_states[i]);

		if (isHistory(state)) {
			// we already did in setHistoryCompletion
			continue;
		}

		NodeSet<std::string> completion;

		if (isParallel(state)) {
			completion = getChildStates(state);

		} else if (state.hasAttribute("initial")) {
			completion = getStates(tokenizeIdRefs(state.getAttribute("initial")));

		} else {
			NodeSet<std::string> initElems = filterChildElements(_nsInfo.xmlNSPrefix + "initial", state);
			if(initElems.size() > 0 && !iequals(ATTR_CAST(initElems[0], "generated"), "true")) {
				// initial element is first child
				completion.push_back(initElems[0]);
			} else {
				// first child state
				Arabica::XPath::NodeSet<std::string> initStates;
				NodeList<std::string> childs = state.getChildNodes();
				for (size_t i = 0; i < childs.getLength(); i++) {
					if (childs.item(i).getNodeType() != Node_base::ELEMENT_NODE)
						continue;
					if (isState(Element<std::string>(childs.item(i)))) {
						completion.push_back(childs.item(i));
						break;
					}
				}
			}
		}

		std::string completionBools;
		for (size_t j = 0; j < _states.size(); j++) {
			if (isMember(_states[j], completion)) {
				completionBools += "1";
			} else {
				completionBools += "0";
			}
		}
		state.setAttribute("completionBools", completionBools);
	}
}

void ChartToC::prepare() {
	_binding = (HAS_ATTR(_scxml, "binding") && iequals(ATTR(_scxml, "binding"), "late") ? LATE : EARLY);
	_name = (HAS_ATTR(_scxml, "name") ? ATTR(_scxml, "name") : "");

	// make sure initial and history elements always precede propoer states
	resortStates(_scxml);

	std::set<std::string> elements;
	elements.insert(_nsInfo.xmlNSPrefix + "scxml");
	elements.insert(_nsInfo.xmlNSPrefix + "state");
	elements.insert(_nsInfo.xmlNSPrefix + "final");
	elements.insert(_nsInfo.xmlNSPrefix + "parallel");
	elements.insert(_nsInfo.xmlNSPrefix + "history");
	elements.insert(_nsInfo.xmlNSPrefix + "initial");
	elements.insert(_nsInfo.xmlNSPrefix + "parallel");
	_states = DOMUtils::inDocumentOrder(elements, _scxml);

	// set states' document order and parent attribute
	for (size_t i = 0; i < _states.size(); i++) {
		Element<std::string> state(_states[i]);
		state.setAttribute("documentOrder", toStr(i));
		if (state.getParentNode().getNodeType() == Node_base::ELEMENT_NODE &&
		        HAS_ATTR_CAST(state.getParentNode(), "documentOrder")) {
			state.setAttribute("parent", ATTR_CAST(state.getParentNode(), "documentOrder"));
		}

		// set the states' children and whether it has a history
		std::string childBools;
		bool hasHistoryChild = false;
		for (size_t j = 0; j < _states.size(); j++) {
			if (_states[j].getParentNode() == state) {
				if (isHistory(Element<std::string>(_states[j]))) {
					hasHistoryChild = true;
				}
				childBools += "1";
			} else {
				childBools += "0";
			}
		}
		state.setAttribute("childBools", childBools);
		if (hasHistoryChild) {
			state.setAttribute("hasHistoryChild", "yes");
		}

		// ancestors
		std::string ancBools;
		for (size_t j = 0; j < _states.size(); j++) {
			if (isDescendant(state, _states[j])) {
				ancBools += "1";
			} else {
				ancBools += "0";
			}
		}
		state.setAttribute("ancBools", ancBools);

	}

	// set transitions' document order and source attribute
	elements.clear();
	elements.insert(_nsInfo.xmlNSPrefix + "transition");
	_transitions = DOMUtils::inDocumentOrder(elements, _scxml);
	for (size_t i = 0; i < _transitions.size(); i++) {
		Element<std::string> transition(_transitions[i]);
		transition.setAttribute("documentOrder", toStr(i));
		if (transition.getParentNode().getNodeType() == Node_base::ELEMENT_NODE &&
		        HAS_ATTR_CAST(transition.getParentNode(), "documentOrder")) {
			transition.setAttribute("source", ATTR_CAST(transition.getParentNode(), "documentOrder"));
		}
	}

	// set transitions' postfix order attribute
	_transitions = DOMUtils::inPostFixOrder(elements, _scxml);
	for (size_t i = 0; i < _transitions.size(); i++) {
		Element<std::string> transition(_transitions[i]);
		transition.setAttribute("postFixOrder", toStr(i));

		// and exit set
		std::string exitSetBools;
		NodeSet<std::string> exitSet = computeExitSet(transition);
		for (unsigned int j = 0; j < _states.size(); j++) {
			Element<std::string> state(_states[j]);
			if (isMember(state, exitSet)) {
				exitSetBools += "1";
			} else {
				exitSetBools += "0";
			}
		}
		transition.setAttribute("exitSetBools", exitSetBools);

		// and conflicts
		std::string conflictBools;
		for (unsigned int j = 0; j < _transitions.size(); j++) {
			Element<std::string> t2(_transitions[j]);
			if (hasIntersection(computeExitSet(transition), computeExitSet(t2)) ||
			        (getSourceState(transition) == getSourceState(t2)) ||
			        (isDescendant(getSourceState(transition), getSourceState(t2))) ||
			        (isDescendant(getSourceState(t2), getSourceState(transition)))) {
				conflictBools += "1";
			} else {
				conflictBools += "0";
			}
		}
		transition.setAttribute("conflictBools", conflictBools);

		// and target
		if (HAS_ATTR(transition, "target")) {
			std::list<std::string> targets = tokenize(ATTR(transition, "target"));

			std::string targetBools;
			for (size_t j = 0; j < _states.size(); j++) {
				Element<std::string> state(_states[j]);

				if (HAS_ATTR(state, "id") &&
				        std::find(targets.begin(), targets.end(), escape(ATTR(state, "id"))) != targets.end()) {
					targetBools += "1";
				} else {
					targetBools += "0";
				}
			}
			transition.setAttribute("targetBools", targetBools);

		}
	}
	// leave transitions in postfix order



	// set the completion of states and responsibility of history elements
	setStateCompletion();

	// how many bits do we need to represent the state array?
	std::string seperator;
	_stateCharArraySize = ceil((float)_states.size() / (float)8);
	_stateCharArrayInit = "{";
	for (size_t i = 0; i < _stateCharArraySize; i++) {
		_stateCharArrayInit += seperator + "0";
		seperator = ", ";
	}
	_stateCharArrayInit += "}";

	if (false) {
	} else if (_states.size() < (1UL << 8)) {
		_stateDataType = "uint8_t";
	} else if (_states.size() < (1UL << 16)) {
		_stateDataType = "uint16_t";
	} else if (_states.size() < (1UL << 32)) {
		_stateDataType = "uint32_t";
	} else {
		_stateDataType = "uint64_t";
	}

	seperator = "";
	_transCharArraySize = ceil((float)_transitions.size() / (float)8);
	_transCharArrayInit = "{";
	for (size_t i = 0; i < _transCharArraySize; i++) {
		_transCharArrayInit += seperator + "0";
		seperator = ", ";
	}
	_transCharArrayInit += "}";

	if (false) {
	} else if (_transitions.size() < (1UL << 8)) {
		_transDataType = "uint8_t";
	} else if (_transitions.size() < (1UL << 16)) {
		_transDataType = "uint16_t";
	} else if (_transitions.size() < (1UL << 32)) {
		_transDataType = "uint32_t";
	} else {
		_transDataType = "uint64_t";
	}

}

void ChartToC::writeTo(std::ostream& stream) {

	writeIncludes(stream);
	writeMacros(stream);
	writeTypes(stream);
	for (std::list<ChartToC*>::iterator machIter = _allMachines.begin(); machIter != _allMachines.end(); machIter++) {
		(*machIter)->writeElementInfo(stream);
		(*machIter)->writeExecContentFinalize(stream);
		(*machIter)->writeElementInfoInvocation(stream);
		(*machIter)->writeExecContent(stream);
		(*machIter)->writeStates(stream);
		(*machIter)->writeTransitions(stream);
	}
	writeMachineInfo(stream);
	writeHelpers(stream);
	writeFSM(stream);

	//    http://stackoverflow.com/questions/2525310/how-to-define-and-work-with-an-array-of-bits-in-c

}

void ChartToC::findNestedMachines() {
	NodeSet<std::string> invokes = InterpreterImpl::filterChildElements(_nsInfo.xmlNSPrefix + "invoke", _scxml, true);

	for (size_t i = 0; i < invokes.size(); i++) {
		if(isInEmbeddedDocument(invokes[i]))
			continue;

		Element<std::string> invoke(invokes[i]);
		if (HAS_ATTR(invoke, "type") &&
		        ATTR(invoke, "type") != "scxml" &&
		        ATTR(invoke, "type") != "http://www.w3.org/TR/scxml/")
			continue;

		ChartToC* c2c = NULL;
		if (HAS_ATTR(invoke, "src")) {
			c2c = new ChartToC(Interpreter::fromURL(ATTR(invoke, "src")));
		} else {
			// is there a nested scxml machine inside?
			NodeSet<std::string> contents = filterChildElements(_nsInfo.xmlNSPrefix + "content", invoke);
			if (contents.size() == 0)
				continue;
			NodeSet<std::string> scxmls = filterChildElements(_nsInfo.xmlNSPrefix + "scxml", contents[0]);
			if (scxmls.size() == 0)
				continue;

			DOMImplementation<std::string> domFactory = Arabica::SimpleDOM::DOMImplementation<std::string>::getDOMImplementation();
			Arabica::DOM::Document<std::string> doc = domFactory.createDocument(_document.getNamespaceURI(), "", 0);
			Node<std::string> imported = doc.importNode(scxmls[0], true);
			doc.appendChild(imported);
//            std::cout << doc << std::endl;
			c2c = new ChartToC(Interpreter::fromDOM(doc, _nsInfo, _sourceURL));
		}

		if (c2c != NULL) {
			invoke.setAttribute("md5sum", c2c->_md5);
			ChartToC* topMostMachine = (_topMostMachine == NULL ? this : _topMostMachine);
			c2c->_topMostMachine = topMostMachine;
			c2c->_parentMachine = this;
			_nestedMachines.push_back(c2c);
			topMostMachine->_allMachines.push_back(c2c);
		}
	}
}

void ChartToC::writeIncludes(std::ostream& stream) {
	stream << "#include <stdint.h> // explicit types" << std::endl;
	stream << "#include <stddef.h> // NULL" << std::endl;
	stream << std::endl;
}

void ChartToC::writeMacros(std::ostream& stream) {
	stream << "#define BIT_HAS(idx, bitset)   ((bitset[idx >> 3] &  (1 << (idx & 7))) != 0)" << std::endl;
	stream << "#define BIT_SET_AT(idx, bitset)  bitset[idx >> 3] |= (1 << (idx & 7));" << std::endl;
	stream << "#define BIT_CLEAR(idx, bitset)   bitset[idx >> 3] &= (1 << (idx & 7)) ^ 0xFF;" << std::endl;
	stream << std::endl;

	stream << "#ifdef __GNUC__" << std::endl;
	stream << "#define likely(x)       (__builtin_expect(!!(x), 1))" << std::endl;
	stream << "#define unlikely(x)     (__builtin_expect(!!(x), 0))" << std::endl;
	stream << "#else" << std::endl;
	stream << "#define likely(x)       (x)" << std::endl;
	stream << "#define unlikely(x)     (x)" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "#ifndef SCXML_NR_STATES_TYPE " << std::endl;
	stream << "#  define SCXML_NR_STATES_TYPE " << _stateDataType << std::endl;
	stream << "#endif " << std::endl;
	stream << std::endl;

	stream << "#ifndef SCXML_NR_TRANS_TYPE " << std::endl;
	stream << "#  define SCXML_NR_TRANS_TYPE " << _stateDataType << std::endl;
	stream << "#endif " << std::endl;
	stream << std::endl;

	stream << "#ifndef SCXML_MAX_NR_STATES_BYTES " << std::endl;
	stream << "#  define SCXML_MAX_NR_STATES_BYTES " << _stateCharArraySize << std::endl;
	stream << "#endif " << std::endl;
	stream << std::endl;

	stream << "#ifndef SCXML_MAX_NR_TRANS_BYTES " << std::endl;
	stream << "#  define SCXML_MAX_NR_TRANS_BYTES " << _transCharArraySize << std::endl;
	stream << "#endif " << std::endl;
	stream << std::endl;

	stream << "// error return codes" << std::endl;
	stream << "#define SCXML_ERR_OK                0" << std::endl;
	stream << "#define SCXML_ERR_IDLE              1" << std::endl;
	stream << "#define SCXML_ERR_DONE              2" << std::endl;
	stream << "#define SCXML_ERR_MISSING_CALLBACK  3" << std::endl;
	stream << "#define SCXML_ERR_FOREACH_DONE      4" << std::endl;
	stream << "#define SCXML_ERR_EXEC_CONTENT      5" << std::endl;
	stream << "#define SCXML_ERR_INVALID_TARGET    6" << std::endl;
	stream << "#define SCXML_ERR_INVALID_TYPE      7" << std::endl;
	stream << "#define SCXML_ERR_UNSUPPORTED       8" << std::endl;
	stream << std::endl;

//	stream << "#define SCXML_NUMBER_STATES " << _states.size() << std::endl;
//	stream << "#define SCXML_NUMBER_TRANS " << _transitions.size() << std::endl;

	stream << "#define SCXML_NUMBER_STATES (ctx->machine->nr_states)" << std::endl;
	stream << "#define SCXML_NUMBER_TRANS (ctx->machine->nr_transitions)" << std::endl;

	stream << std::endl;

	stream << "#define SCXML_TRANS_SPONTANEOUS      0x01" << std::endl;
	stream << "#define SCXML_TRANS_TARGETLESS       0x02" << std::endl;
	stream << "#define SCXML_TRANS_INTERNAL         0x04" << std::endl;
	stream << "#define SCXML_TRANS_HISTORY          0x08" << std::endl;
	stream << "#define SCXML_TRANS_INITIAL          0x10" << std::endl;
	stream << std::endl;

	stream << "#define SCXML_STATE_ATOMIC           0x01" << std::endl;
	stream << "#define SCXML_STATE_PARALLEL         0x02" << std::endl;
	stream << "#define SCXML_STATE_COMPOUND         0x03" << std::endl;
	stream << "#define SCXML_STATE_FINAL            0x04" << std::endl;
	stream << "#define SCXML_STATE_HISTORY_DEEP     0x05" << std::endl;
	stream << "#define SCXML_STATE_HISTORY_SHALLOW  0x06" << std::endl;
	stream << "#define SCXML_STATE_INITIAL          0x07" << std::endl;
	stream << "#define SCXML_STATE_HAS_HISTORY      0x80 // highest bit" << std::endl;
	stream << "#define SCXML_STATE_MASK(t)          (t & 0x7F) // mask highest bit" << std::endl;

	stream << "" << std::endl;
	stream << "#define SCXML_CTX_PRISTINE           0x00" << std::endl;
	stream << "#define SCXML_CTX_SPONTANEOUS        0x01" << std::endl;
	stream << "#define SCXML_CTX_INITIALIZED        0x02" << std::endl;
	stream << "#define SCXML_CTX_TOP_LEVEL_FINAL    0x04" << std::endl;
	stream << "#define SCXML_CTX_TRANSITION_FOUND   0x08" << std::endl;
	stream << std::endl;



	stream << "#define ELEM_DATA_IS_SET(data) (data->id != NULL)" << std::endl;
	stream << "#define ELEM_DONEDATA_IS_SET(donedata) (donedata->content != NULL || donedata->contentexpr != NULL || donedata->params != NULL)" << std::endl;
	stream << "#define ELEM_PARAM_IS_SET(param) (param->name != NULL)" << std::endl;
	stream << "#define SCXML_MACHINE_IS_SET(machine) (machine->nr_states > 0)" << std::endl;
	stream << std::endl;
}

void ChartToC::writeTypes(std::ostream& stream) {

	stream << std::endl;
	stream << "typedef struct scxml_machine scxml_machine;" << std::endl;
	stream << "typedef struct scxml_transition scxml_transition;" << std::endl;
	stream << "typedef struct scxml_state scxml_state;" << std::endl;
	stream << "typedef struct scxml_ctx scxml_ctx;" << std::endl;
	stream << "typedef struct scxml_elem_invoke scxml_elem_invoke;" << std::endl;
	stream << std::endl;

	stream << "typedef struct scxml_elem_send scxml_elem_send;" << std::endl;
	stream << "typedef struct scxml_elem_param scxml_elem_param;" << std::endl;
	stream << "typedef struct scxml_elem_data scxml_elem_data;" << std::endl;
	stream << "typedef struct scxml_elem_donedata scxml_elem_donedata;" << std::endl;
	stream << "typedef struct scxml_elem_foreach scxml_elem_foreach;" << std::endl;
	stream << std::endl;

	stream << "typedef void* (*dequeue_internal_t)(const scxml_ctx* ctx);" << std::endl;
	stream << "typedef void* (*dequeue_external_t)(const scxml_ctx* ctx);" << std::endl;
	stream << "typedef int (*is_enabled_t)(const scxml_ctx* ctx, const scxml_transition* transition, const void* event);" << std::endl;
	stream << "typedef int (*is_true_t)(const scxml_ctx* ctx, const char* expr);" << std::endl;
	stream << "typedef int (*exec_content_t)(const scxml_ctx* ctx, const scxml_state* state, const void* event);" << std::endl;
	stream << "typedef int (*raise_done_event_t)(const scxml_ctx* ctx, const scxml_state* state, const scxml_elem_donedata* donedata);" << std::endl;
	stream << "typedef int (*invoke_t)(const scxml_ctx* ctx, const scxml_state* s, const scxml_elem_invoke* invocation, uint8_t uninvoke);" << std::endl;
	stream << std::endl;

	stream << "typedef int (*exec_content_log_t)(const scxml_ctx* ctx, const char* label, const char* expr);" << std::endl;
	stream << "typedef int (*exec_content_raise_t)(const scxml_ctx* ctx, const char* event);" << std::endl;
	stream << "typedef int (*exec_content_send_t)(const scxml_ctx* ctx, const scxml_elem_send* send);" << std::endl;
	stream << "typedef int (*exec_content_foreach_init_t)(const scxml_ctx* ctx, const scxml_elem_foreach* foreach);" << std::endl;
	stream << "typedef int (*exec_content_foreach_next_t)(const scxml_ctx* ctx, const scxml_elem_foreach* foreach);" << std::endl;
	stream << "typedef int (*exec_content_foreach_done_t)(const scxml_ctx* ctx, const scxml_elem_foreach* foreach);" << std::endl;
	stream << "typedef int (*exec_content_assign_t)(const scxml_ctx* ctx, const char* location, const char* expr);" << std::endl;
	stream << "typedef int (*exec_content_init_t)(const scxml_ctx* ctx, const scxml_elem_data* data);" << std::endl;
	stream << "typedef int (*exec_content_cancel_t)(const scxml_ctx* ctx, const char* sendid, const char* sendidexpr);" << std::endl;
	stream << "typedef int (*exec_content_finalize_t)(const scxml_ctx* ctx, const scxml_elem_invoke* invoker, const void* event);" << std::endl;
	stream << "typedef int (*exec_content_script_t)(const scxml_ctx* ctx, const char* src, const char* content);" << std::endl;
	stream << std::endl;

#if 1
	stream << "struct scxml_machine {" << std::endl;
	stream << "    uint8_t                    flags;" << std::endl;
	stream << "    SCXML_NR_STATES_TYPE       nr_states;" << std::endl;
	stream << "    SCXML_NR_TRANS_TYPE        nr_transitions;" << std::endl;
	stream << "    const char*                name;" << std::endl;
	stream << "    const char*                datamodel;" << std::endl;
	stream << "    const char*                uuid;" << std::endl;
	stream << "    const scxml_state*         states;" << std::endl;
	stream << "    const scxml_transition*    transitions;" << std::endl;
	stream << "    const scxml_machine*       parent;" << std::endl;
	stream << "    const scxml_elem_donedata* donedata;" << std::endl;
	stream << "    const exec_content_t       script;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "// forward declare machines to allow references" << std::endl;
	stream << "extern const scxml_machine scxml_machines[" << toStr(_allMachines.size() + 1) << "];" << std::endl;
	stream << std::endl;
#endif

	stream << "struct scxml_elem_data {" << std::endl;
	stream << "    const char* id;" << std::endl;
	stream << "    const char* src;" << std::endl;
	stream << "    const char* expr;" << std::endl;
	stream << "    const char* content;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_state {" << std::endl;
	stream << "    const char* name; // eventual name" << std::endl;
	stream << "    const " << _stateDataType << " parent; // parent" << std::endl;
	stream << "    const exec_content_t on_entry; // on entry handlers" << std::endl;
	stream << "    const exec_content_t on_exit; // on exit handlers" << std::endl;
	stream << "    const invoke_t invoke; // invocations" << std::endl;
	stream << "    const char children[SCXML_MAX_NR_STATES_BYTES]; // all children" << std::endl;
	stream << "    const char completion[SCXML_MAX_NR_STATES_BYTES]; // default completion" << std::endl;
	stream << "    const char ancestors[SCXML_MAX_NR_STATES_BYTES]; // all ancestors" << std::endl;
	stream << "    const scxml_elem_data* data;" << std::endl;
	stream << "    const uint8_t type; // atomic, parallel, compound, final, history" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_transition {" << std::endl;
	stream << "    const " << _stateDataType << " source;" << std::endl;
	stream << "    const char target[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    const char* event;" << std::endl;
	stream << "    const char* condition;" << std::endl;
	stream << "    const exec_content_t on_transition;" << std::endl;
	stream << "    const uint8_t type;" << std::endl;
	stream << "    const char conflicts[SCXML_MAX_NR_TRANS_BYTES];" << std::endl;
	stream << "    const char exit_set[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_elem_foreach {" << std::endl;
	stream << "    const char* array;" << std::endl;
	stream << "    const char* item;" << std::endl;
	stream << "    const char* index;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_elem_param {" << std::endl;
	stream << "    const char* name;" << std::endl;
	stream << "    const char* expr;" << std::endl;
	stream << "    const char* location;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_elem_donedata {" << std::endl;
	stream << "    const " << _stateDataType << " source;" << std::endl;
	stream << "    const char* content;" << std::endl;
	stream << "    const char* contentexpr;" << std::endl;
	stream << "    const scxml_elem_param* params;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_elem_invoke {" << std::endl;
	stream << "    const scxml_machine* machine;" << std::endl;
	stream << "    const char* type;" << std::endl;
	stream << "    const char* typeexpr;" << std::endl;
	stream << "    const char* src;" << std::endl;
	stream << "    const char* srcexpr;" << std::endl;
	stream << "    const char* id;" << std::endl;
	stream << "    const char* idlocation;" << std::endl;
	stream << "    const char* namelist;" << std::endl;
	stream << "    const uint8_t autoforward;" << std::endl;
	stream << "    const scxml_elem_param* params;" << std::endl;
	stream << "    exec_content_finalize_t finalize;" << std::endl;
	stream << "    const char* content;" << std::endl;
	stream << "    const char* contentexpr;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_elem_send {" << std::endl;
	stream << "    const char* event;" << std::endl;
	stream << "    const char* eventexpr;" << std::endl;
	stream << "    const char* target;" << std::endl;
	stream << "    const char* targetexpr;" << std::endl;
	stream << "    const char* type;" << std::endl;
	stream << "    const char* typeexpr;" << std::endl;
	stream << "    const char* id;" << std::endl;
	stream << "    const char* idlocation;" << std::endl;
	stream << "    const char* delay;" << std::endl;
	stream << "    const char* delayexpr;" << std::endl;
	stream << "    const char* namelist;" << std::endl;
	stream << "    const char* content;" << std::endl;
	stream << "    const char* contentexpr;" << std::endl;
	stream << "    const scxml_elem_param* params;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

	stream << "struct scxml_ctx {" << std::endl;
	stream << "    uint8_t        flags;" << std::endl;
	stream << "    const scxml_machine* machine;" << std::endl;
	stream << std::endl;
	stream << "    char config[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char history[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char invocations[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char initialized_data[SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << std::endl;
	stream << "    void* user_data;" << std::endl;
	stream << "    void* event;" << std::endl;
	stream << std::endl;
	stream << "    dequeue_internal_t dequeue_internal;" << std::endl;
	stream << "    dequeue_external_t dequeue_external;" << std::endl;
	stream << "    is_enabled_t is_enabled;" << std::endl;
	stream << "    is_true_t is_true;" << std::endl;
	stream << "    raise_done_event_t raise_done_event;" << std::endl;
	stream << std::endl;
	stream << "    exec_content_log_t exec_content_log;" << std::endl;
	stream << "    exec_content_raise_t exec_content_raise;" << std::endl;
	stream << "    exec_content_send_t exec_content_send;" << std::endl;
	stream << "    exec_content_foreach_init_t exec_content_foreach_init;" << std::endl;
	stream << "    exec_content_foreach_next_t exec_content_foreach_next;" << std::endl;
	stream << "    exec_content_foreach_done_t exec_content_foreach_done;" << std::endl;
	stream << "    exec_content_assign_t exec_content_assign;" << std::endl;
	stream << "    exec_content_init_t exec_content_init;" << std::endl;
	stream << "    exec_content_cancel_t exec_content_cancel;" << std::endl;
	stream << "    exec_content_script_t exec_content_script;" << std::endl;
	stream << "    invoke_t invoke;" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;
}

void ChartToC::writeHelpers(std::ostream& stream) {
	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "static void printStateNames(const scxml_ctx* ctx, const char* a, size_t length) {" << std::endl;
	stream << "    size_t i;" << std::endl;
	stream << "    const char* seperator = \"\";" << std::endl;
	stream << "    for (i = 0; i < length; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, a)) {" << std::endl;
	stream << "            printf(\"%s%s\", seperator, (ctx->machine->states[i].name != NULL ? ctx->machine->states[i].name : \"UNK\"));" << std::endl;
	stream << "            seperator = \", \";" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << "    printf(\"\\n\");" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void printBitsetIndices(const char* a, size_t length) {" << std::endl;
	stream << "    size_t i;" << std::endl;
	stream << "    const char* seperator = \"\";" << std::endl;
	stream << "    for (i = 0; i < length; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, a)) {" << std::endl;
	stream << "            printf(\"%s%lu\", seperator, i);" << std::endl;
	stream << "            seperator = \", \";" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << "    printf(\"\\n\");" << std::endl;
	stream << "}" << std::endl;

	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "static int bit_has_and(const char* a, const char* b, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        if (a[i] & b[i])" << std::endl;
	stream << "            return 1;" << std::endl;
	stream << "    }" << std::endl;
	stream << "    return 0;" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void bit_clear_all(char* a, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        a[i] = 0;" << std::endl;
	stream << "    }" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static int bit_has_any(const char* a, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        if (a[i] > 0)" << std::endl;
	stream << "            return 1;" << std::endl;
	stream << "    }" << std::endl;
	stream << "    return 0;" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void bit_or(char* dest, const char* mask, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        dest[i] |= mask[i];" << std::endl;
	stream << "    }" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void bit_copy(char* dest, const char* source, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        dest[i] = source[i];" << std::endl;
	stream << "    }" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void bit_and_not(char* dest, const char* mask, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        dest[i] &= ~mask[i];" << std::endl;
	stream << "    }" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

	stream << "static void bit_and(char* dest, const char* mask, size_t i) {" << std::endl;
	stream << "    while(i--) {" << std::endl;
	stream << "        dest[i] &= mask[i];" << std::endl;
	stream << "    };" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;

}

void ChartToC::writeExecContentFinalize(std::ostream& stream) {
	// needs to be written prior to invocation elem info
	NodeSet<std::string> finalizes = filterChildElements(_nsInfo.xmlNSPrefix + "finalize", _scxml, true);
	for (size_t i = 0; i < finalizes.size(); i++) {
		Element<std::string> finalize(finalizes[i]);
		NodeSet<std::string> execContent = filterChildType(Node_base::ELEMENT_NODE, finalize);

		if (execContent.size() > 0) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(finalize) << "(const scxml_ctx* ctx, const scxml_elem_invoke* invocation, const void* event) {" << std::endl;
			stream << "    int err = SCXML_ERR_OK;" << std::endl;
			for (size_t j = 0; j < execContent.size(); j++) {
				writeExecContent(stream, Element<std::string>(execContent[j]), 1);
			}
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}
	}

}

void ChartToC::writeExecContent(std::ostream& stream) {
	for (size_t i = 0; i < _states.size(); i++) {
		Element<std::string> state(_states[i]);

		if (i == 0) {
			// root state - we need to perform some initialization here
			NodeSet<std::string> globalScripts = filterChildElements(_nsInfo.xmlNSPrefix + "script", state);
			if (globalScripts.size() > 0) {
				for (size_t j = 0; j < globalScripts.size(); j++) {
					stream << "static int " << _prefix << "_global_script_" << toStr(j) << "(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
					stream << "    int err = SCXML_ERR_OK;" << std::endl;
					writeExecContent(stream, globalScripts[j], 1);
					stream << "    return SCXML_ERR_OK;" << std::endl;
					stream << "}" << std::endl;
				}

				stream << "static int " << _prefix << "_global_script(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
				for (size_t j = 0; j < globalScripts.size(); j++) {
					stream << "    " << _prefix << "_global_script_" << toStr(j) << "(ctx, state, event);" << std::endl;
				}
				stream << "    return SCXML_ERR_OK;" << std::endl;
				stream << "}" << std::endl;
				stream << std::endl;
			}
		}

		NodeSet<std::string> onexit = filterChildElements(_nsInfo.xmlNSPrefix + "onexit", state);
		for (size_t j = 0; j < onexit.size(); j++) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_exit_" << toStr(j) << "(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
			stream << "    int err = SCXML_ERR_OK;" << std::endl;
			writeExecContent(stream, onexit[j], 1);
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}

		if (onexit.size() > 0) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_exit(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
			for (size_t j = 0; j < onexit.size(); j++) {
				stream << "    " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_exit_" << toStr(j) << "(ctx, state, event);" << std::endl;
			}
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}


		NodeSet<std::string> onentry = filterChildElements(_nsInfo.xmlNSPrefix + "onentry", state);
		for (size_t j = 0; j < onentry.size(); j++) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_entry_" << toStr(j) << "(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
			stream << "    int err = SCXML_ERR_OK;" << std::endl;
			writeExecContent(stream, onentry[j], 1);
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}

		if (onentry.size() > 0) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_entry(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
			for (size_t j = 0; j < onentry.size(); j++) {
				stream << "    " << _prefix << "_" << DOMUtils::idForNode(state) << "_on_entry_" << toStr(j) << "(ctx, state, event);" << std::endl;
			}

			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}


		NodeSet<std::string> invokes = filterChildElements(_nsInfo.xmlNSPrefix + "invoke", state);
		if (invokes.size() > 0) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(state) << "_invoke(const scxml_ctx* ctx, const scxml_state* s, const scxml_elem_invoke* invocation, uint8_t uninvoke) {" << std::endl;
			for (size_t j = 0; j < invokes.size(); j++) {
				Element<std::string> invoke(invokes[j]);
				stream << "    ctx->invoke(ctx, s, &" << _prefix << "_elem_invokes[" << ATTR(invoke, "documentOrder") << "], uninvoke);" << std::endl;
				stream << std::endl;
			}
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
		}
	}

	for (size_t i = 0; i < _transitions.size(); i++) {
		Element<std::string> transition(_transitions[i]);
		NodeSet<std::string> execContent = filterChildType(Node_base::ELEMENT_NODE, transition);

		if (execContent.size() > 0) {
			stream << "static int " << _prefix << "_" << DOMUtils::idForNode(transition) << "_on_trans(const scxml_ctx* ctx, const scxml_state* state, const void* event) {" << std::endl;
			stream << "    int err = SCXML_ERR_OK;" << std::endl;
			for (size_t j = 0; j < execContent.size(); j++) {
				writeExecContent(stream, Element<std::string>(execContent[j]), 1);
			}
			stream << "    return SCXML_ERR_OK;" << std::endl;
			stream << "}" << std::endl;
			stream << std::endl;
		}
	}

}

void ChartToC::writeExecContent(std::ostream& stream, const Arabica::DOM::Node<std::string>& node, int indent) {
	if (!node)
		return;

	if (node.getNodeType() == Node_base::TEXT_NODE) {
		if (boost::trim_copy(node.getNodeValue()).length() > 0) {
			std::string escaped = escape(node.getNodeValue());
			stream << escaped;
		}
		return;
	}

	std::string padding;
	for (size_t i = 0; i < indent; i++) {
		padding += "    ";
	}


	if (node.getNodeType() != Node_base::ELEMENT_NODE)
		return; // skip anything not an element

	Arabica::DOM::Element<std::string> elem = Arabica::DOM::Element<std::string>(node);

	if (false) {
	} else if(TAGNAME(elem) == "onentry" || TAGNAME(elem) == "onexit" || TAGNAME(elem) == "transition" || TAGNAME(elem) == "finalize") {
		// descent into childs and write their contents
		Arabica::DOM::Node<std::string> child = node.getFirstChild();
		while(child) {
			writeExecContent(stream, child, indent);
			child = child.getNextSibling();
		}
	} else if(TAGNAME(elem) == "script") {
		stream << padding;
		stream << "if likely(ctx->exec_content_script != NULL) {" << std::endl;
		stream << padding;
		stream << "    if unlikely((err = ctx->exec_content_script(ctx, ";
		stream << (HAS_ATTR(elem, "src") ? "\"" + escape(ATTR(elem, "src")) + "\"" : "NULL") << ", ";

		NodeSet<std::string> scriptTexts = filterChildType(Node_base::TEXT_NODE, elem);
		if (scriptTexts.size() > 0) {
			stream << "\"";
			writeExecContent(stream, scriptTexts[0], 0);
			stream << "\"";
		} else {
			stream << "NULL";
		}

		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "log") {
		stream << padding;
		stream << "if likely(ctx->exec_content_log != NULL) {" << std::endl;
		stream << padding;
		stream << "    if unlikely((ctx->exec_content_log(ctx, ";
		stream << (HAS_ATTR(elem, "label") ? "\"" + escape(ATTR(elem, "label")) + "\"" : "NULL") << ", ";
		stream << (HAS_ATTR(elem, "expr") ? "\"" + escape(ATTR(elem, "expr")) + "\"" : "NULL");
		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "foreach") {
		stream << padding << "if likely(ctx->exec_content_foreach_init != NULL &&" << std::endl;
		stream << padding << "          ctx->exec_content_foreach_next != NULL &&" << std::endl;
		stream << padding << "          ctx->exec_content_foreach_done != NULL) {" << std::endl;
		stream << std::endl;

		stream << padding << "    if unlikely((ctx->exec_content_foreach_init(ctx, &" << _prefix << "_elem_foreachs[" << ATTR(elem, "documentOrder") << "])) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "    while (ctx->exec_content_foreach_next(ctx, &" << _prefix << "_elem_foreachs[" << ATTR(elem, "documentOrder") << "]) == SCXML_ERR_OK) {" << std::endl;
		Arabica::DOM::Node<std::string> child = node.getFirstChild();
		while(child) {
			writeExecContent(stream, child, indent + 2);
			child = child.getNextSibling();
		}
		stream << padding << "    }" << std::endl;
		stream << padding << "    if ((ctx->exec_content_foreach_done(ctx, &" << _prefix << "_elem_foreachs[" << ATTR(elem, "documentOrder") << "])) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "if") {
		stream << padding;
		stream << "if likely(ctx->is_true != NULL) {" << std::endl;
		stream << padding;
		stream << "    if (ctx->is_true(ctx, " << (HAS_ATTR(elem, "cond") ? "\"" + escape(ATTR(elem, "cond")) + "\"" : "NULL") << ")) {" << std::endl;
		Arabica::DOM::Node<std::string> child = elem.getFirstChild();
		while(child) {
			if (child.getNodeType() == Node_base::ELEMENT_NODE && TAGNAME_CAST(child) == "elseif") {
				stream << padding;
				stream << "    } else if (ctx->is_true(ctx, " << (HAS_ATTR_CAST(child, "cond") ? "\"" + escape(ATTR_CAST(child, "cond")) + "\"" : "NULL") << ")) {" << std::endl;
			} else if (child.getNodeType() == Node_base::ELEMENT_NODE && TAGNAME_CAST(child) == "else") {
				stream << padding;
				stream << "    } else {" << std::endl;
			} else {
				writeExecContent(stream, child, indent + 2);
			}
			child = child.getNextSibling();
		}
		stream << padding << "    }" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "assign") {
		stream << padding;
		stream << "if likely(ctx->exec_content_assign != NULL) {" << std::endl;
		stream << padding;
		stream << "    if ((ctx->exec_content_assign(ctx, ";
		stream << (HAS_ATTR(elem, "location") ? "\"" + escape(ATTR(elem, "location")) + "\"" : "NULL") << ", ";
		if (HAS_ATTR(elem, "expr")) {
			stream << "\"" + escape(ATTR(elem, "expr")) + "\"";
		} else {
			NodeSet<std::string> assignTexts = filterChildType(Node_base::TEXT_NODE, elem);
			if (assignTexts.size() > 0) {
				stream << "\"";
				writeExecContent(stream, assignTexts[0], 0);
				stream << "\"";
			} else {
				stream << "NULL";
			}
		}
		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;


	} else if(TAGNAME(elem) == "raise") {
		stream << padding;
		stream << "if likely(ctx->exec_content_raise != NULL) {" << std::endl;
		stream << padding;
		stream << "    if unlikely((ctx->exec_content_raise(ctx, ";
		stream << (HAS_ATTR(elem, "event") ? "\"" + escape(ATTR(elem, "event")) + "\"" : "NULL");
		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "send") {
		stream << padding;
		stream << "if likely(ctx->exec_content_send != NULL) {" << std::endl;
		stream << padding;
		stream << "    if ((ctx->exec_content_send(ctx, &" << _prefix << "_elem_sends[" << ATTR(elem, "documentOrder") << "]";
		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else if(TAGNAME(elem) == "cancel") {
		stream << padding;
		stream << "if likely(ctx->exec_content_cancel != NULL) {" << std::endl;
		stream << padding;
		stream << "    if ((ctx->exec_content_cancel(ctx, ";
		stream << (HAS_ATTR(elem, "sendid") ? "\"" + escape(ATTR(elem, "sendid")) + "\"" : "NULL") << ", ";
		stream << (HAS_ATTR(elem, "sendidexpr") ? "\"" + escape(ATTR(elem, "sendidexpr")) + "\"" : "NULL");
		stream << ")) != SCXML_ERR_OK) return err;" << std::endl;
		stream << padding << "} else {" << std::endl;
		stream << padding << "    return SCXML_ERR_MISSING_CALLBACK;" << std::endl;
		stream << padding << "}" << std::endl;

	} else {
		std::cerr << "'" << TAGNAME(elem) << "'" << std::endl << elem << std::endl;
		assert(false);
	}

}

void ChartToC::writeElementInfoInvocation(std::ostream& stream) {
	NodeSet<std::string> invokes = filterChildElements(_nsInfo.xmlNSPrefix + "invoke", _scxml, true);
	if (invokes.size() > 0) {
		_hasElement.insert("invoke");
		stream << "static const scxml_elem_invoke " << _prefix << "_elem_invokes[" << invokes.size() << "] = {" << std::endl;
		for (size_t i = 0; i < invokes.size(); i++) {
			Element<std::string> invoke(invokes[i]);

			/*
			 stream << "struct scxml_elem_invoke {" << std::endl;
			 stream << "    const char* machine;" << std::endl;
			 stream << "    const char* type;" << std::endl;
			 stream << "    const char* typeexpr;" << std::endl;
			 stream << "    const char* src;" << std::endl;
			 stream << "    const char* srcexpr;" << std::endl;
			 stream << "    const char* id;" << std::endl;
			 stream << "    const char* idlocation;" << std::endl;
			 stream << "    const char* namelist;" << std::endl;
			 stream << "    const uint8_t autoforward;" << std::endl;
			 stream << "    const scxml_elem_param* params;" << std::endl;
			 stream << "    const exec_content_finalize_t* finalize;" << std::endl;
			 stream << "    const char* content;" << std::endl;
			 stream << "    const char* contentexpr;" << std::endl;
			 stream << "};" << std::endl;
			 */

			stream << "    { " << std::endl;

			stream << "        /* machine     */ ";
			if (HAS_ATTR(invoke, "md5sum")) {
				size_t machIdx = 0;
				for (std::list<ChartToC*>::iterator machIter = _allMachines.begin(); machIter != _allMachines.end(); machIter++, machIdx++) {
					if ((*machIter)->_md5 == ATTR(invoke, "md5sum")) {
						stream << "&scxml_machines[" << toStr(machIdx) << "]";
						break;
					}
				}
			} else {
				stream << "NULL";
			}
			stream << ", " << std::endl;

			stream << "        /* type        */ ";
			stream << (HAS_ATTR(invoke, "type") ? "\"" +  escape(ATTR(invoke, "type")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* typeexpr    */ ";
			stream << (HAS_ATTR(invoke, "typeexpr") ? "\"" +  escape(ATTR(invoke, "typeexpr")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* src         */ ";
			stream << (HAS_ATTR(invoke, "src") ? "\"" + escape(ATTR(invoke, "src")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* srcexpr     */ ";
			stream << (HAS_ATTR(invoke, "srcexpr") ? "\"" +  escape(ATTR(invoke, "srcexpr")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* id          */ ";
			stream << (HAS_ATTR(invoke, "id") ? "\"" +  escape(ATTR(invoke, "id")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* idlocation  */ ";
			stream << (HAS_ATTR(invoke, "idlocation") ? "\"" +  escape(ATTR(invoke, "idlocation")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* namelist    */ ";
			stream << (HAS_ATTR(invoke, "namelist") ? "\"" +  escape(ATTR(invoke, "namelist")) + "\"" : "NULL");
			stream << ", " << std::endl;

			stream << "        /* autoforward */ ";
			stream << (HAS_ATTR(invoke, "autoforward") && stringIsTrue(ATTR(invoke, "autoforward")) ? "1" : "0");
			stream << ", " << std::endl;

			stream << "        /* params      */ ";
			if (HAS_ATTR(invoke, "paramIndex")) {
				stream << "&" << _prefix << "_elem_params[" << escape(ATTR(invoke, "paramIndex")) << "]";
			} else {
				stream << "NULL";
			}
			stream << ", " << std::endl;

			stream << "        /* finalize    */ ";
			NodeSet<std::string> finalizes = filterChildElements(_nsInfo.xmlNSPrefix + "finalize", invoke);
			if (finalizes.size() > 0) {
				stream << _prefix << "_" << DOMUtils::idForNode(finalizes[0]);
			} else {
				stream << "NULL";
			}
			stream << ", " << std::endl;

			NodeSet<std::string> contents = filterChildElements(_nsInfo.xmlNSPrefix + "content", invoke);
			if (contents.size() > 0 && !HAS_ATTR(invoke, "md5sum")) {
				std::stringstream ss;
				NodeList<std::string> cChilds = contents[0].getChildNodes();
				for (size_t j = 0; j < cChilds.getLength(); j++) {
					ss << cChilds.item(j);
				}
				stream << "        /* content      */ ";
				stream << (ss.str().size() > 0 ? "\"" + escape(ss.str()) + "\", " : "NULL, ") << std::endl;
				stream << "        /* contentexpr  */ ";
				stream << (HAS_ATTR_CAST(contents[0], "expr") ? "\"" + ATTR_CAST(contents[0], "expr") + "\", " : "NULL, ") << std::endl;
			} else {
				stream << "        /* content     */ NULL," << std::endl;
				stream << "        /* contentexpr */ NULL," << std::endl;
			}

			stream << "    }" << (i + 1 < invokes.size() ? ",": "") << std::endl;
			invoke.setAttribute("documentOrder", toStr(i));

		}
		stream << "};" << std::endl;
		stream << std::endl;
	}

}

void ChartToC::writeElementInfo(std::ostream& stream) {
	NodeSet<std::string> foreachs = filterChildElements(_nsInfo.xmlNSPrefix + "foreach", _scxml, true);
	if (foreachs.size() > 0) {
		_hasElement.insert("foreach");
		stream << "static const scxml_elem_foreach " << _prefix << "_elem_foreachs[" << foreachs.size() << "] = {" << std::endl;
		stream << "    /* array, item, index */" << std::endl;
		for (size_t i = 0; i < foreachs.size(); i++) {
			Element<std::string> foreach(foreachs[i]);
			stream << "    { ";
			stream << (HAS_ATTR(foreach, "array") ? "\"" + escape(ATTR(foreach, "array")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(foreach, "item") ? "\"" + escape(ATTR(foreach, "item")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(foreach, "index") ? "\"" + escape(ATTR(foreach, "index")) + "\"" : "NULL");
			stream << " }" << (i + 1 < foreachs.size() ? ",": "") << std::endl;
			foreach.setAttribute("documentOrder", toStr(i));
		}
		stream << "};" << std::endl;
		stream << std::endl;
	}

	NodeSet<std::string> datas = filterChildElements(_nsInfo.xmlNSPrefix + "data", _scxml, true);
	if (datas.size() > 0) {
		_hasElement.insert("data");
		size_t dataIndexOffset = 0;
		Node<std::string> parent;
		size_t distinctParents = 0;

		if (_binding == InterpreterImpl::EARLY) {
			Element<std::string>(_states[0]).setAttribute("dataIndex", "0");
			distinctParents = 1;
		} else {
			for (size_t i = 0; i < datas.size(); i++) {
				Element<std::string> data(datas[i]);
				if (data.getParentNode() != parent) {
					distinctParents++;
				}
			}
		}

		parent = Node<std::string>();

		stream << "static const scxml_elem_data " << _prefix << "_elem_datas[" << datas.size() + distinctParents << "] = {" << std::endl;
		stream << "    /* id, src, expr, content */" << std::endl;
		for (size_t i = 0; i < datas.size(); i++) {
			Element<std::string> data(datas[i]);
			if (data.getParentNode().getParentNode() != parent) {
				if (_binding == InterpreterImpl::LATE) {
					if (i > 0) {
						stream << "    { NULL, NULL, NULL, NULL }," << std::endl;
						dataIndexOffset++;
					}
					Element<std::string>(data.getParentNode().getParentNode()).setAttribute("dataIndex", toStr(i + dataIndexOffset));
				}
				parent = data.getParentNode().getParentNode();
			}
			stream << "    { ";
			stream << (HAS_ATTR(data, "id") ? "\"" + escape(ATTR(data, "id")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(data, "src") ? "\"" + escape(ATTR(data, "src")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(data, "expr") ? "\"" + escape(ATTR(data, "expr")) + "\"" : "NULL") << ", ";

			NodeSet<std::string> dataTexts = filterChildType(Node_base::TEXT_NODE, data);
			if (dataTexts.size() > 0) {
				if (boost::trim_copy(dataTexts[0].getNodeValue()).length() > 0) {
					std::string escaped = escape(dataTexts[0].getNodeValue());
					stream << "\"" << escaped << "\"" << std::endl;
				}
			} else {
				stream << "NULL";
			}
			stream << " }," << std::endl;

		}
		stream << "    { NULL, NULL, NULL, NULL }" << std::endl;
		stream << "};" << std::endl;
		stream << std::endl;
	}

	NodeSet<std::string> params = filterChildElements(_nsInfo.xmlNSPrefix + "param", _scxml, true);
	if (params.size() > 0) {
		_hasElement.insert("param");
		Node<std::string> parent;
		size_t distinctParents = 0;
		for (size_t i = 0; i < params.size(); i++) {
			Element<std::string> param(params[i]);
			if (param.getParentNode() != parent) {
				distinctParents++;
			}
		}
		parent = Node<std::string>();

		stream << "static const scxml_elem_param " << _prefix << "_elem_params[" << params.size() + distinctParents << "] = {" << std::endl;
		stream << "    /* name, expr, location */" << std::endl;
		for (size_t i = 0; i < params.size(); i++) {
			Element<std::string> param(params[i]);
			if (param.getParentNode() != parent) {
				Element<std::string>(param.getParentNode()).setAttribute("paramIndex", toStr(i));
				if (i > 0) {
					stream << "    { NULL, NULL, NULL }," << std::endl;
				}
				parent = param.getParentNode();
			}
			stream << "    { ";
			stream << (HAS_ATTR(param, "name") ? "\"" + escape(ATTR(param, "name")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(param, "expr") ? "\"" + escape(ATTR(param, "expr")) + "\"" : "NULL") << ", ";
			stream << (HAS_ATTR(param, "location") ? "\"" + escape(ATTR(param, "location")) + "\"" : "NULL");
			stream << " }," << std::endl;

		}
		stream << "    { NULL, NULL, NULL }" << std::endl;
		stream << "};" << std::endl;
		stream << std::endl;
	}

	NodeSet<std::string> sends = filterChildElements(_nsInfo.xmlNSPrefix + "send", _scxml, true);
	if (sends.size() > 0) {
		_hasElement.insert("send");
		stream << "static const scxml_elem_send " << _prefix << "_elem_sends[" << sends.size() << "] = {" << std::endl;
		for (size_t i = 0; i < sends.size(); i++) {
			Element<std::string> send(sends[i]);
			stream << "    { ";
			stream << std::endl << "        /* event       */ ";
			stream << (HAS_ATTR(send, "event") ? "\"" + escape(ATTR(send, "event")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* eventexpr   */ ";
			stream << (HAS_ATTR(send, "eventexpr") ? "\"" + escape(ATTR(send, "eventexpr")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* target      */ ";
			stream << (HAS_ATTR(send, "target") ? "\"" + escape(ATTR(send, "target")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* targetexpr  */ ";
			stream << (HAS_ATTR(send, "targetexpr") ? "\"" + escape(ATTR(send, "targetexpr")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* type        */ ";
			stream << (HAS_ATTR(send, "type") ? "\"" + escape(ATTR(send, "type")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* typeexpr    */ ";
			stream << (HAS_ATTR(send, "typeexpr") ? "\"" + escape(ATTR(send, "typeexpr")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* id          */ ";
			stream << (HAS_ATTR(send, "id") ? "\"" + escape(ATTR(send, "id")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* idlocation  */ ";
			stream << (HAS_ATTR(send, "idlocation") ? "\"" + escape(ATTR(send, "idlocation")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* delay       */ ";
			stream << (HAS_ATTR(send, "delay") ? "\"" + escape(ATTR(send, "delay")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* delayexpr   */ ";
			stream << (HAS_ATTR(send, "delayexpr") ? "\"" + escape(ATTR(send, "delayexpr")) + "\"" : "NULL") << ", ";
			stream << std::endl << "        /* namelist    */ ";
			stream << (HAS_ATTR(send, "namelist") ? "\"" + escape(ATTR(send, "namelist")) + "\"" : "NULL") << ", ";

			NodeSet<std::string> contents = filterChildElements(_nsInfo.xmlNSPrefix + "content", send);
			if (contents.size() > 0) {
				std::stringstream ss;
				NodeList<std::string> cChilds = contents[0].getChildNodes();
				for (size_t j = 0; j < cChilds.getLength(); j++) {
					ss << cChilds.item(j);
				}
				stream << std::endl << "        /* content     */ ";
				stream << (ss.str().size() > 0 ? "\"" + escape(ss.str()) + "\", " : "NULL, ");
				stream << std::endl << "        /* contentexpr  */ ";
				stream << (HAS_ATTR_CAST(contents[0], "expr") ? "\"" + ATTR_CAST(contents[0], "expr") + "\", " : "NULL, ");
			} else {
				stream << std::endl << "        /* content     */ ";
				stream << "NULL,";
				stream << std::endl << "        /* contentexpr */ ";
				stream << "NULL,";
			}


			stream << std::endl << "        /* params      */ ";
			if (HAS_ATTR(send, "paramIndex")) {
				stream << "&" << _prefix << "_elem_params[" << escape(ATTR(send, "paramIndex")) << "] ";
			} else {
				stream << "NULL ";
			}

			stream << std::endl << "    }" << (i + 1 < sends.size() ? ",": "") << std::endl;
			send.setAttribute("documentOrder", toStr(i));
		}
		stream << "};" << std::endl;
		stream << std::endl;
	}

	NodeSet<std::string> donedatas = filterChildElements(_nsInfo.xmlNSPrefix + "donedata", _scxml, true);
	stream << "static const scxml_elem_donedata " << _prefix << "_elem_donedatas[" << donedatas.size() + 1 << "] = {" << std::endl;
	stream << "    /* source, content, contentexpr, params */" << std::endl;
	for (size_t i = 0; i < donedatas.size(); i++) {
		_hasElement.insert("donedata");
		Element<std::string> donedata(donedatas[i]);
		stream << "    { ";

		// parent
		stream << ATTR_CAST(donedata.getParentNode(), "documentOrder") << ", ";

		NodeSet<std::string> contents = filterChildElements(_nsInfo.xmlNSPrefix + "content", donedata);
		if (contents.size() > 0) {
			std::stringstream ss;
			NodeList<std::string> cChilds = contents[0].getChildNodes();
			for (size_t j = 0; j < cChilds.getLength(); j++) {
				ss << cChilds.item(j);
			}
			stream << (ss.str().size() > 0 ? "\"" + escape(ss.str()) + "\", " : "NULL, ");
			stream << (HAS_ATTR_CAST(contents[0], "expr") ? "\"" + ATTR_CAST(contents[0], "expr") + "\", " : "NULL, ");
		} else {
			stream << "NULL, NULL, ";
		}

		if (HAS_ATTR(donedata, "paramIndex")) {
			stream << "&" << _prefix << "_elem_params[" << escape(ATTR(donedata, "paramIndex")) << "]";
		} else {
			stream << "NULL";
		}

		stream << " }," << std::endl;
		donedata.setAttribute("documentOrder", toStr(i));
	}
	stream << "    { 0, NULL, NULL, NULL }" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;

}

void ChartToC::writeMachineInfo(std::ostream& stream) {
	if (_topMostMachine != NULL)
		return;

	stream << "const scxml_machine scxml_machines[" << _allMachines.size() + 1<< "] = {" << std::endl;
	for (std::list<ChartToC*>::iterator machineIter = _allMachines.begin(); machineIter != _allMachines.end(); machineIter++) {
		ChartToC* m = (*machineIter);
		stream << "    {" << std::endl;
		stream << "        /* flags          */ 0," << std::endl;
		stream << "        /* nr_states      */ " << m->_states.size() << "," << std::endl;
		stream << "        /* nr_transitions */ " << m->_transitions.size() << "," << std::endl;
		stream << "        /* name           */ \"" << escape(m->_name) << "\"," << std::endl;
		stream << "        /* datamodel      */ \"" << (HAS_ATTR(m->_scxml, "datamodel") ? ATTR(m->_scxml, "datamodel") : "null") << "\"," << std::endl;
		stream << "        /* uuid           */ \"" << m->_md5 << "\"," << std::endl;
		stream << "        /* states         */ " << "&" << m->_prefix << "_states[0], " << std::endl;
		stream << "        /* transitions    */ " << "&" << m->_prefix << "_transitions[0], " << std::endl;
		stream << "        /* parent         */ ";
		if (m->_parentMachine != NULL) {
			size_t parentIndex = 0;
			for (std::list<ChartToC*>::iterator parentIter = _allMachines.begin(); parentIter != _allMachines.end(); parentIter++, parentIndex++) {
				if (*parentIter == m->_parentMachine) {
					stream << "&scxml_machines[" << toStr(parentIndex) << "]";
				}
			}
		} else {
			stream << "NULL";
		}
		stream << "," << std::endl;

		stream << "        /* donedata       */ " << "&" << m->_prefix << "_elem_donedatas[0], " << std::endl;
		stream << "        /* script         */ ";
		if (filterChildElements(_nsInfo.xmlNSPrefix + "script", _scxml).size() > 0) {
			stream << m->_prefix << "_global_script" << std::endl;
		} else {
			stream << "NULL";
		}
		stream << std::endl;

		stream << "    }," << std::endl;

	}
	stream << "    {0, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL }" << std::endl;
	stream << "};" << std::endl;
	stream << std::endl;
}

void ChartToC::writeStates(std::ostream& stream) {
	stream << "static const scxml_state " << _prefix << "_states[" << toStr(_states.size()) << "] = {" << std::endl;
	for (size_t i = 0; i < _states.size(); i++) {
		Element<std::string> state(_states[i]);

		stream << "    {   /* state number " << toStr(i) << " */" << std::endl;

		// name
		stream << "        /* name       */ ";
		stream << (HAS_ATTR(state, "id") ? "\"" + escape(ATTR(state, "id")) + "\"" : "NULL");
		stream << "," << std::endl;

		// parent
		stream << "        /* parent     */ ";
		stream << (i == 0 ? "0" : ATTR_CAST(state.getParentNode(), "documentOrder"));
		stream << "," << std::endl;

		// onentry
		stream << "        /* onentry    */ ";
		stream << (filterChildElements(_nsInfo.xmlNSPrefix + "onentry", state).size() > 0 ? _prefix + "_" + DOMUtils::idForNode(state) + "_on_entry" : "NULL");
		stream << "," << std::endl;

		// onexit
		stream << "        /* onexit     */ ";
		stream << (filterChildElements(_nsInfo.xmlNSPrefix + "onexit", state).size() > 0 ? _prefix + "_" + DOMUtils::idForNode(state) + "_on_exit" : "NULL");
		stream << "," << std::endl;

		// invokers
		stream << "        /* invoke     */ ";
		stream << (filterChildElements(_nsInfo.xmlNSPrefix + "invoke", state).size() > 0 ? _prefix + "_" + DOMUtils::idForNode(state) + "_invoke" : "NULL");
		stream << "," << std::endl;

		// children
		stream << "        /* children   */ { ";
		writeCharArrayInitList(stream, ATTR(state, "childBools"));
		stream << " /* " << ATTR(state, "childBools") << " */ }," << std::endl;

		// default completion
		stream << "        /* completion */ { ";
		writeCharArrayInitList(stream, ATTR(state, "completionBools"));
		stream << " /* " << ATTR(state, "completionBools") << " */ }, \t" << std::endl;

		stream << "        /* ancestors  */ { ";
		writeCharArrayInitList(stream, ATTR(state, "ancBools"));
		stream << " /* " << ATTR(state, "ancBools") << " */ }," << std::endl;

		stream << "        /* data       */ ";
		stream << (HAS_ATTR(state, "dataIndex") ? "&" + _prefix + "_elem_datas[" + escape(ATTR(state, "dataIndex")) + "]" : "NULL");
		stream << "," << std::endl;

		stream << "        /* type       */ ";

		if (false) {
		} else if (iequals(TAGNAME(state), "initial")) {
			stream << "SCXML_STATE_INITIAL";
		} else if (isFinal(state)) {
			stream << "SCXML_STATE_FINAL";
		} else if (isHistory(state)) {
			if (HAS_ATTR(state, "type") && iequals(ATTR(state, "type"), "deep")) {
				stream << "SCXML_STATE_HISTORY_DEEP";
			} else {
				stream << "SCXML_STATE_HISTORY_SHALLOW";
			}
		} else if (isAtomic(state)) {
			stream << "SCXML_STATE_ATOMIC";
		} else if (isParallel(state)) {
			stream << "SCXML_STATE_PARALLEL";
		} else if (isCompound(state)) {
			stream << "SCXML_STATE_COMPOUND";
		} else { // <scxml>
			stream << "SCXML_STATE_COMPOUND";
		}
		if (HAS_ATTR(state, "hasHistoryChild")) {
			stream << " | SCXML_STATE_HAS_HISTORY";
		}

		stream << "," << std::endl;

		stream << "    }" << (i + 1 < _states.size() ? ",": "") << std::endl;
	}
	stream << "};" << std::endl;
	stream << std::endl;
}


void ChartToC::writeTransitions(std::ostream& stream) {

	// cross reference transition by document order - is this really needed?!
	std::set<std::string> elements;
	elements.insert(_nsInfo.xmlNSPrefix + "transition");
	NodeSet<std::string> transDocOrder = DOMUtils::inDocumentOrder(elements, _scxml);

	stream << "static const scxml_transition " << _prefix << "_transitions[" << toStr(_transitions.size()) << "] = {" << std::endl;
	for (size_t i = 0; i < _transitions.size(); i++) {
		Element<std::string> transition(_transitions[i]);

		stream << "    {   /* transition number " << ATTR(transition, "documentOrder") << " with priority " << toStr(i) << std::endl;
		stream << "           target: " << ATTR(transition, "target") << std::endl;
		stream << "         */" << std::endl;

		// source
		stream << "        /* source     */ ";
		stream << ATTR_CAST(transition.getParentNode(), "documentOrder");
		stream << "," << std::endl;

		// targets
		stream << "        /* target     */ ";
		if (HAS_ATTR(transition, "targetBools")) {
			stream << "{ ";
			writeCharArrayInitList(stream, ATTR(transition, "targetBools"));
			stream << " /* " << ATTR(transition, "targetBools") << " */ }";

		} else {
			stream << "{ NULL }";
		}
		stream << "," << std::endl;

		stream << "        /* event      */ ";
		stream << (HAS_ATTR(transition, "event") ? "\"" + escape(ATTR(transition, "event")) + "\"" : "NULL");
		stream << "," << std::endl;

		stream << "        /* condition  */ ";
		stream << (HAS_ATTR(transition, "cond") ? "\"" + escape(ATTR(transition, "cond")) + "\"" : "NULL");
		stream << "," << std::endl;

		// on transition handlers
		stream << "        /* ontrans    */ ";
		if (filterChildType(Arabica::DOM::Node_base::ELEMENT_NODE, transition).size() > 0) {
			stream << _prefix << "_" << DOMUtils::idForNode(transition) + "_on_trans";
		} else {
			stream << "NULL";
		}
		stream << "," << std::endl;

		// type
		stream << "        /* type       */ ";
		std::string seperator = "";
		if (!HAS_ATTR(transition, "target")) {
			stream << seperator << "SCXML_TRANS_TARGETLESS";
			seperator = " | ";
		}

		if (HAS_ATTR(transition, "type") && iequals(ATTR(transition, "type"), "internal")) {
			stream << seperator << "SCXML_TRANS_INTERNAL";
			seperator = " | ";
		}

		if (!HAS_ATTR(transition, "event")) {
			stream << seperator << "SCXML_TRANS_SPONTANEOUS";
			seperator = " | ";
		}

		if (iequals(TAGNAME_CAST(transition.getParentNode()), "history")) {
			stream << seperator << "SCXML_TRANS_HISTORY";
			seperator = " | ";
		}

		if (iequals(TAGNAME_CAST(transition.getParentNode()), "initial")) {
			stream << seperator << "SCXML_TRANS_INITIAL";
			seperator = " | ";
		}

		if (seperator.size() == 0) {
			stream << "0";
		}
		stream << "," << std::endl;

		// conflicts
		stream << "        /* conflicts  */ { ";
		writeCharArrayInitList(stream, ATTR(transition, "conflictBools"));
		stream << " /* " << ATTR(transition, "conflictBools") << " */ }, " << std::endl;

		// exit set
		stream << "        /* exit set   */ { ";
		writeCharArrayInitList(stream, ATTR(transition, "exitSetBools"));
		stream << " /* " << ATTR(transition, "exitSetBools") << " */ }" << std::endl;

		stream << "    }" << (i + 1 < _transitions.size() ? ",": "") << std::endl;
	}
	stream << "};" << std::endl;
	stream << std::endl;
}

Arabica::XPath::NodeSet<std::string> ChartToC::computeExitSet(const Arabica::DOM::Element<std::string>& transition) {

	NodeSet<std::string> statesToExit;
	if (!isTargetless(transition)) {
		Arabica::DOM::Node<std::string> domain = getTransitionDomain(transition);
		if (!domain)
			return statesToExit;
		for (unsigned int j = 0; j < _states.size(); j++) {
			const Node<std::string>& s = _states[j];
			if (isDescendant(s, domain)) {
				statesToExit.push_back(s);
			}
		}
	}

	return statesToExit;
}

void ChartToC::writeCharArrayInitList(std::ostream& stream, const std::string& boolString) {
	/**
	 * 0111 -> 0x08
	 * 1111 -> 0x0f
	 * 1111 1111 -> 0xff
	 * 1111 1111 1110 -> 0x0f, 0xfd
	 *
	 * 76543210 fedcba98 ...
	 */

	std::string charArray;
	size_t index = 0;
	char currChar = 0;

	for (std::string::const_iterator bIter = boolString.begin(); bIter != boolString.end(); bIter++) {

		if (*bIter == '1') {
			currChar |= 1 << index;
		}

		index++;
		if (index == 8) {
			charArray += currChar;
			currChar = 0;
			index = 0;
		}
	}

	if (index != 0) {
		charArray += currChar;
	}

	std::string seperator = "";
	std::ios::fmtflags f(stream.flags());

	for (std::string::const_iterator cIter = charArray.begin(); cIter != charArray.end(); cIter++) {
		stream << seperator << "0x" << std::setw(2) << std::setfill('0') << std::hex << int(*cIter & 0xFF);
		seperator = ", ";
	}
	stream.flags(f);
}

void ChartToC::writeFSM(std::ostream& stream) {
	stream << "int scxml_step(scxml_ctx* ctx) {" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"Config: \");" << std::endl;
	stream << "    printStateNames(ctx, ctx->config, SCXML_NUMBER_STATES);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "    if (ctx->flags & SCXML_CTX_TOP_LEVEL_FINAL) " << std::endl;
	stream << "        return SCXML_ERR_DONE; " << std::endl;
	stream << std::endl;

	stream << "    " << (_states.size() > _transitions.size() ? "SCXML_NR_STATES_TYPE" : "SCXML_NR_TRANS_TYPE") << " i, j, k;" << std::endl;
	stream << "    SCXML_NR_STATES_TYPE nr_states_bytes = ((SCXML_NUMBER_STATES + 7) & ~7) >> 3;" << std::endl;
	stream << "    SCXML_NR_TRANS_TYPE  nr_trans_bytes  = ((SCXML_NUMBER_TRANS + 7) & ~7) >> 3;" << std::endl;
	stream << "    int err = SCXML_ERR_OK;" << std::endl;

	stream << "    char conflicts  [SCXML_MAX_NR_TRANS_BYTES];" << std::endl;
	stream << "    char trans_set  [SCXML_MAX_NR_TRANS_BYTES];" << std::endl;
	stream << "    char target_set [SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char exit_set   [SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char entry_set  [SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << "    char tmp_states [SCXML_MAX_NR_STATES_BYTES];" << std::endl;
	stream << std::endl;

	stream << "    bit_clear_all(target_set, nr_states_bytes);" << std::endl;
	stream << "    bit_clear_all(trans_set, nr_trans_bytes);" << std::endl;
	stream << "    if unlikely(ctx->flags == SCXML_CTX_PRISTINE) {" << std::endl;
	stream << "        if (ctx->machine->script != NULL)" << std::endl;
	stream << "            ctx->machine->script(ctx, &ctx->machine->states[0], NULL);" << std::endl;
	stream << "        bit_or(target_set, ctx->machine->states[0].completion, nr_states_bytes);" << std::endl;
	stream << "        ctx->flags |= SCXML_CTX_SPONTANEOUS | SCXML_CTX_INITIALIZED;" << std::endl;
	stream << "        goto ESTABLISH_ENTRY_SET;" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "    if (ctx->flags & SCXML_CTX_SPONTANEOUS) {" << std::endl;
	stream << "        ctx->event = NULL;" << std::endl;
	stream << "        goto SELECT_TRANSITIONS;" << std::endl;
	stream << "    }" << std::endl;
	stream << "    if ((ctx->event = ctx->dequeue_internal(ctx)) != NULL) {" << std::endl;
	stream << "        goto SELECT_TRANSITIONS;" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "    // manage invocations" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_STATES; i++) {" << std::endl;
	stream << "        // uninvoke" << std::endl;
	stream << "        if (!BIT_HAS(i, ctx->config) && BIT_HAS(i, ctx->invocations)) {" << std::endl;
	stream << "            if (ctx->machine->states[i].invoke != NULL)" << std::endl;
	stream << "                ctx->machine->states[i].invoke(ctx, &ctx->machine->states[i], NULL, 1);" << std::endl;
	stream << "            BIT_CLEAR(i, ctx->invocations)" << std::endl;
	stream << "        }" << std::endl;
	stream << "        // invoke" << std::endl;
	stream << "        if (BIT_HAS(i, ctx->config) && !BIT_HAS(i, ctx->invocations)) {" << std::endl;
	stream << "            if (ctx->machine->states[i].invoke != NULL)" << std::endl;
	stream << "                ctx->machine->states[i].invoke(ctx, &ctx->machine->states[i], NULL, 0);" << std::endl;
	stream << "            BIT_SET_AT(i, ctx->invocations)" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "    if ((ctx->event = ctx->dequeue_external(ctx)) != NULL) {" << std::endl;
	stream << "        goto SELECT_TRANSITIONS;" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "SELECT_TRANSITIONS:" << std::endl;
	stream << "    bit_clear_all(conflicts, nr_trans_bytes);" << std::endl;
	stream << "    bit_clear_all(exit_set, nr_states_bytes);" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_TRANS; i++) {" << std::endl;
	stream << "        // never select history or initial transitions automatically" << std::endl;
	stream << "        if unlikely(ctx->machine->transitions[i].type & (SCXML_TRANS_HISTORY | SCXML_TRANS_INITIAL))" << std::endl;
	stream << "            continue;" << std::endl;
	stream << std::endl;
	stream << "        // is the transition active?" << std::endl;
	stream << "        if (BIT_HAS(ctx->machine->transitions[i].source, ctx->config)) {" << std::endl;
	stream << "            // is it non-conflicting?" << std::endl;
	stream << "            if (!BIT_HAS(i, conflicts)) {" << std::endl;
	stream << "                // is it enabled?" << std::endl;
	stream << "                if (ctx->is_enabled(ctx, &ctx->machine->transitions[i], ctx->event) > 0) {" << std::endl;
	stream << "                    // remember that we found a transition" << std::endl;
	stream << "                    ctx->flags |= SCXML_CTX_TRANSITION_FOUND;" << std::endl;
	stream << std::endl;

	stream << "                    // transitions that are pre-empted" << std::endl;
	stream << "                    bit_or(conflicts, ctx->machine->transitions[i].conflicts, nr_trans_bytes);" << std::endl;
	stream << std::endl;
	stream << "                    // states that are directly targeted (resolve as entry-set later)" << std::endl;
	stream << "                    bit_or(target_set, ctx->machine->transitions[i].target, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "                    // states that will be left" << std::endl;
	stream << "                    bit_or(exit_set, ctx->machine->transitions[i].exit_set, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "                    BIT_SET_AT(i, trans_set);" << std::endl;
	stream << "                }" << std::endl;
	stream << "            }" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << "    bit_and(exit_set, ctx->config, nr_states_bytes);" << std::endl;
	stream << std::endl;

	stream << "    if (ctx->flags & SCXML_CTX_TRANSITION_FOUND) {" << std::endl;
	stream << "        ctx->flags |= SCXML_CTX_SPONTANEOUS;" << std::endl;
	stream << "        ctx->flags &= ~SCXML_CTX_TRANSITION_FOUND;" << std::endl;
	stream << "    } else {" << std::endl;
	stream << "        ctx->flags &= ~SCXML_CTX_SPONTANEOUS;" << std::endl;
//	stream << "        return SCXML_ERR_OK;" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"Targets: \");" << std::endl;
	stream << "    printStateNames(ctx, target_set, SCXML_NUMBER_STATES);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"Exiting: \");" << std::endl;
	stream << "    printStateNames(ctx, exit_set, SCXML_NUMBER_STATES);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"History: \");" << std::endl;
	stream << "    printStateNames(ctx, ctx->history, SCXML_NUMBER_STATES);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "// REMEMBER_HISTORY:" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_STATES; i++) {" << std::endl;
	stream << "        if unlikely(SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_HISTORY_SHALLOW ||" << std::endl;
	stream << "                    SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_HISTORY_DEEP) {" << std::endl;
	stream << "            // a history state whose parent is about to be exited" << std::endl;
	stream << "            if unlikely(BIT_HAS(ctx->machine->states[i].parent, exit_set)) {" << std::endl;
	stream << "                bit_copy(tmp_states, ctx->machine->states[i].completion, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "                // set those states who were enabled" << std::endl;
	stream << "                bit_and(tmp_states, ctx->config, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "                // clear current history with completion mask" << std::endl;
	stream << "                bit_and_not(ctx->history, ctx->machine->states[i].completion, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "                // set history" << std::endl;
	stream << "                bit_or(ctx->history, tmp_states, nr_states_bytes);" << std::endl;
	stream << "            }" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "ESTABLISH_ENTRY_SET:" << std::endl;
	stream << "    // calculate new entry set" << std::endl;
	stream << "    bit_copy(entry_set, target_set, nr_states_bytes);" << std::endl;
	stream << std::endl;
	stream << "    // iterate for ancestors" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_STATES; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, entry_set)) {" << std::endl;
	stream << "            bit_or(entry_set, ctx->machine->states[i].ancestors, nr_states_bytes);" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "    // iterate for descendants" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_STATES; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, entry_set)) {" << std::endl;
	stream << "            switch (SCXML_STATE_MASK(ctx->machine->states[i].type)) {" << std::endl;
	stream << "                case SCXML_STATE_PARALLEL: {" << std::endl;
	stream << "                    bit_or(entry_set, ctx->machine->states[i].completion, nr_states_bytes);" << std::endl;
	stream << "                    break;" << std::endl;
	stream << "                }" << std::endl;
	stream << "                case SCXML_STATE_HISTORY_SHALLOW:" << std::endl;
	stream << "                case SCXML_STATE_HISTORY_DEEP: {" << std::endl;
	stream << "                    if (!bit_has_and(ctx->machine->states[i].completion, ctx->history, nr_states_bytes) &&" << std::endl;
	stream << "                        !BIT_HAS(ctx->machine->states[i].parent, ctx->config)) {" << std::endl;
	stream << "                        // nothing set for history, look for a default transition" << std::endl;
	stream << "                        for (j = 0; j < SCXML_NUMBER_TRANS; j++) {" << std::endl;
	stream << "                            if unlikely(ctx->machine->transitions[j].source == i) {" << std::endl;
	stream << "                                bit_or(entry_set, ctx->machine->transitions[j].target, nr_states_bytes);" << std::endl;
	stream << "                                if(SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_HISTORY_DEEP &&" << std::endl;
	stream << "                                   !bit_has_and(ctx->machine->transitions[j].target, ctx->machine->states[i].children, nr_states_bytes)) {" << std::endl;
	stream << "                                    for (k = i + 1; k < SCXML_NUMBER_STATES; k++) {" << std::endl;
	stream << "                                        if (BIT_HAS(k, ctx->machine->transitions[j].target)) {" << std::endl;
	stream << "                                            bit_or(entry_set, ctx->machine->states[k].ancestors, nr_states_bytes);" << std::endl;
	stream << "                                            break;" << std::endl;
	stream << "                                        }" << std::endl;
	stream << "                                    }" << std::endl;
	stream << "                                }" << std::endl;
	stream << "                                BIT_SET_AT(j, trans_set);" << std::endl;
	stream << "                                break;" << std::endl;
	stream << "                            }" << std::endl;
	stream << "                            // Note: SCXML mandates every history to have a transition!" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                    } else {" << std::endl;
	stream << "                        bit_copy(tmp_states, ctx->machine->states[i].completion, nr_states_bytes);" << std::endl;
	stream << "                        bit_and(tmp_states, ctx->history, nr_states_bytes);" << std::endl;
	stream << "                        bit_or(entry_set, tmp_states, nr_states_bytes);" << std::endl;
	stream << "                        if (ctx->machine->states[i].type == (SCXML_STATE_HAS_HISTORY | SCXML_STATE_HISTORY_DEEP)) {" << std::endl;
	stream << "                            // a deep history state with nested histories -> more completion" << std::endl;
	stream << "                            for (j = i + 1; j < SCXML_NUMBER_STATES; j++) {" << std::endl;
	stream << "                                if (BIT_HAS(j, ctx->machine->states[i].completion) &&" << std::endl;
	stream << "                                    BIT_HAS(j, entry_set) &&" << std::endl;
	stream << "                                    (ctx->machine->states[j].type & SCXML_STATE_HAS_HISTORY)) {" << std::endl;
	stream << "                                    for (k = j + 1; k < SCXML_NUMBER_STATES; k++) {" << std::endl;
	stream << "                                        // add nested history to entry_set" << std::endl;
	stream << "                                        if ((SCXML_STATE_MASK(ctx->machine->states[k].type) == SCXML_STATE_HISTORY_DEEP ||" << std::endl;
	stream << "                                             SCXML_STATE_MASK(ctx->machine->states[k].type) == SCXML_STATE_HISTORY_SHALLOW) &&" << std::endl;
	stream << "                                            BIT_HAS(k, ctx->machine->states[j].children)) {" << std::endl;
	stream << "                                            // a nested history state" << std::endl;
	stream << "                                            BIT_SET_AT(k, entry_set);" << std::endl;
	stream << "                                        }" << std::endl;
	stream << "                                    }" << std::endl;
	stream << "                                }" << std::endl;
	stream << "                            }" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                    break;" << std::endl;
	stream << "                }" << std::endl;
	stream << "                case SCXML_STATE_INITIAL: {" << std::endl;
	stream << "                    for (j = 0; j < SCXML_NUMBER_TRANS; j++) {" << std::endl;
	stream << "                        if (ctx->machine->transitions[j].source == i) {" << std::endl;
	stream << "                            BIT_SET_AT(j, trans_set);" << std::endl;
	stream << "                            BIT_CLEAR(i, entry_set);" << std::endl;
	stream << "                            bit_or(entry_set, ctx->machine->transitions[j].target, nr_states_bytes);" << std::endl;
	stream << "                            for (k = i + 1; k < SCXML_NUMBER_STATES; k++) {" << std::endl;
	stream << "                                if (BIT_HAS(k, ctx->machine->transitions[j].target)) {" << std::endl;
	stream << "                                    bit_or(entry_set, ctx->machine->states[k].ancestors, nr_states_bytes);" << std::endl;
	stream << "                                }" << std::endl;
	stream << "                            }" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                    break;" << std::endl;
	stream << "                }" << std::endl;
	stream << "                case SCXML_STATE_COMPOUND: { // we need to check whether one child is already in entry_set" << std::endl;
	stream << "                    if (!bit_has_and(entry_set, ctx->machine->states[i].children, nr_states_bytes) &&" << std::endl;
	stream << "                        (!bit_has_and(ctx->config, ctx->machine->states[i].children, nr_states_bytes) ||" << std::endl;
	stream << "                         bit_has_and(exit_set, ctx->machine->states[i].children, nr_states_bytes)))" << std::endl;
	stream << "                    {" << std::endl;
	stream << "                        bit_or(entry_set, ctx->machine->states[i].completion, nr_states_bytes);" << std::endl;
	stream << "                        if (!bit_has_and(ctx->machine->states[i].completion, ctx->machine->states[i].children, nr_states_bytes)) {" << std::endl;
	stream << "                            // deep completion" << std::endl;
	stream << "                            for (j = i + 1; j < SCXML_NUMBER_STATES; j++) {" << std::endl;
	stream << "                                if (BIT_HAS(j, ctx->machine->states[i].completion)) {" << std::endl;
	stream << "                                    bit_or(entry_set, ctx->machine->states[j].ancestors, nr_states_bytes);" << std::endl;
	stream << "                                    break; // completion of compound is single state" << std::endl;
	stream << "                                }" << std::endl;
	stream << "                            }" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                    break;" << std::endl;
	stream << "                }" << std::endl;
	stream << "            }" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"Transitions: \");" << std::endl;
	stream << "    printBitsetIndices(trans_set, sizeof(char) * 8 * nr_trans_bytes);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "// EXIT_STATES:" << std::endl;
	stream << "    i = SCXML_NUMBER_STATES;" << std::endl;
	stream << "    while(i-- > 0) {" << std::endl;
	stream << "        if (BIT_HAS(i, exit_set) && BIT_HAS(i, ctx->config)) {" << std::endl;
	stream << "            // call all on exit handlers" << std::endl;
	stream << "            if (ctx->machine->states[i].on_exit != NULL) {" << std::endl;
	stream << "                if unlikely((err = ctx->machine->states[i].on_exit(ctx, &ctx->machine->states[i], ctx->event)) != SCXML_ERR_OK)" << std::endl;
	stream << "                    return err;" << std::endl;
	stream << "            }" << std::endl;
	stream << "            BIT_CLEAR(i, ctx->config);" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "// TAKE_TRANSITIONS:" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_TRANS; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, trans_set) && (ctx->machine->transitions[i].type & (SCXML_TRANS_HISTORY | SCXML_TRANS_INITIAL)) == 0) {" << std::endl;
	stream << "            // call executable content in transition" << std::endl;
	stream << "            if (ctx->machine->transitions[i].on_transition != NULL) {" << std::endl;
	stream << "                if unlikely((err = ctx->machine->transitions[i].on_transition(ctx," << std::endl;
	stream << "                                                                              &ctx->machine->states[ctx->machine->transitions[i].source]," << std::endl;
	stream << "                                                                              ctx->event)) != SCXML_ERR_OK)" << std::endl;
	stream << "                    return err;" << std::endl;
	stream << "            }" << std::endl;
	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "#ifdef SCXML_VERBOSE" << std::endl;
	stream << "    printf(\"Entering: \");" << std::endl;
	stream << "    printStateNames(ctx, entry_set, SCXML_NUMBER_STATES);" << std::endl;
	stream << "#endif" << std::endl;
	stream << std::endl;

	stream << "// ENTER_STATES:" << std::endl;
	stream << "    for (i = 0; i < SCXML_NUMBER_STATES; i++) {" << std::endl;
	stream << "        if (BIT_HAS(i, entry_set) && !BIT_HAS(i, ctx->config)) {" << std::endl;
	stream << "            // these are no proper states" << std::endl;
	stream << "            if unlikely(SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_HISTORY_DEEP ||" << std::endl;
	stream << "                        SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_HISTORY_SHALLOW ||" << std::endl;
	stream << "                        SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_INITIAL)" << std::endl;
	stream << "                continue;" << std::endl;
	stream << std::endl;

	stream << "            BIT_SET_AT(i, ctx->config);" << std::endl;
	stream << std::endl;

	stream << "            // initialize data" << std::endl;
	stream << "            if (!BIT_HAS(i, ctx->initialized_data)) {" << std::endl;
	stream << "                if unlikely(ctx->machine->states[i].data != NULL && ctx->exec_content_init != NULL) {" << std::endl;
	stream << "                    ctx->exec_content_init(ctx, ctx->machine->states[i].data);" << std::endl;
	stream << "                }" << std::endl;
	stream << "                BIT_SET_AT(i, ctx->initialized_data);" << std::endl;
	stream << "            }" << std::endl;
	stream << std::endl;

	stream << "            if (ctx->machine->states[i].on_entry != NULL) {" << std::endl;
	stream << "                if unlikely((err = ctx->machine->states[i].on_entry(ctx, &ctx->machine->states[i], ctx->event)) != SCXML_ERR_OK)" << std::endl;
	stream << "                    return err;" << std::endl;
	stream << "            }" << std::endl;
	stream << std::endl;

	stream << "            // take history and initial transitions" << std::endl;
	stream << "            for (j = 0; j < SCXML_NUMBER_TRANS; j++) {" << std::endl;
	stream << "                if unlikely(BIT_HAS(j, trans_set) &&" << std::endl;
	stream << "                            (ctx->machine->transitions[j].type & (SCXML_TRANS_HISTORY | SCXML_TRANS_INITIAL)) &&" << std::endl;
	stream << "                            ctx->machine->states[ctx->machine->transitions[j].source].parent == i) {" << std::endl;
	stream << "                    // call executable content in transition" << std::endl;
	stream << "                    if (ctx->machine->transitions[j].on_transition != NULL) {" << std::endl;
	stream << "                        if unlikely((err = ctx->machine->transitions[j].on_transition(ctx," << std::endl;
	stream << "                                                                                      &ctx->machine->states[i]," << std::endl;
	stream << "                                                                                      ctx->event)) != SCXML_ERR_OK)" << std::endl;
	stream << "                            return err;" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                }" << std::endl;
	stream << "            }" << std::endl;
	stream << std::endl;

	stream << "            // handle final states" << std::endl;
	stream << "            if unlikely(SCXML_STATE_MASK(ctx->machine->states[i].type) == SCXML_STATE_FINAL) {" << std::endl;
	stream << "                if unlikely(ctx->machine->states[i].ancestors[0] == 0x01) {" << std::endl;
	stream << "                    ctx->flags |= SCXML_CTX_TOP_LEVEL_FINAL;" << std::endl;
	stream << "                } else {" << std::endl;
	stream << "                    // raise done event" << std::endl;
	stream << "                    const scxml_elem_donedata* donedata = &ctx->machine->donedata[0];" << std::endl;
	stream << "                    while(ELEM_DONEDATA_IS_SET(donedata)) {" << std::endl;
	stream << "                        if unlikely(donedata->source == i)" << std::endl;
	stream << "                            break;" << std::endl;
	stream << "                        donedata++;" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                    ctx->raise_done_event(ctx, &ctx->machine->states[ctx->machine->states[i].parent], (ELEM_DONEDATA_IS_SET(donedata) ? donedata : NULL));" << std::endl;
	stream << "                }" << std::endl;
	stream << std::endl;

	stream << "                /**" << std::endl;
	stream << "                 * are we the last final state to leave a parallel state?:" << std::endl;
	stream << "                 * 1. Gather all parallel states in our ancestor chain" << std::endl;
	stream << "                 * 2. Find all states for which these parallels are ancestors" << std::endl;
	stream << "                 * 3. Iterate all active final states and remove their ancestors" << std::endl;
	stream << "                 * 4. If a state remains, not all children of a parallel are final" << std::endl;
	stream << "                 */" << std::endl;
	stream << "                for (j = 0; j < SCXML_NUMBER_STATES; j++) {" << std::endl;
	stream << "                    if unlikely(SCXML_STATE_MASK(ctx->machine->states[j].type) == SCXML_STATE_PARALLEL &&" << std::endl;
	stream << "                                BIT_HAS(j, ctx->machine->states[i].ancestors)) {" << std::endl;
	stream << "                        bit_clear_all(tmp_states, nr_states_bytes);" << std::endl;
	stream << "                        for (k = 0; k < SCXML_NUMBER_STATES; k++) {" << std::endl;
	stream << "                            if unlikely(BIT_HAS(j, ctx->machine->states[k].ancestors) && BIT_HAS(k, ctx->config)) {" << std::endl;
	stream << "                                if (SCXML_STATE_MASK(ctx->machine->states[k].type) == SCXML_STATE_FINAL) {" << std::endl;
	stream << "                                    bit_and_not(tmp_states, ctx->machine->states[k].ancestors, nr_states_bytes);" << std::endl;
	stream << "                                } else {" << std::endl;
	stream << "                                    BIT_SET_AT(k, tmp_states);" << std::endl;
	stream << "                                }" << std::endl;
	stream << "                            }" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                        if unlikely(!bit_has_any(tmp_states, nr_states_bytes)) {" << std::endl;
	stream << "                            ctx->raise_done_event(ctx, &ctx->machine->states[j], NULL);" << std::endl;
	stream << "                        }" << std::endl;
	stream << "                    }" << std::endl;
	stream << "                }" << std::endl;
	stream << std::endl;

	stream << "            }" << std::endl;
	stream << std::endl;

	stream << "        }" << std::endl;
	stream << "    }" << std::endl;
	stream << std::endl;

	stream << "    return SCXML_ERR_OK;" << std::endl;
	stream << "}" << std::endl;
	stream << std::endl;
}

ChartToC::~ChartToC() {
}

}