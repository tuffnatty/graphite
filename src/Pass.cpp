/*  GRAPHITENG LICENSING

    Copyright 2010, SIL International
    All rights reserved.

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should also have received a copy of the GNU Lesser General Public
    License along with this library in the file named "LICENSE".
    If not, write to the Free Software Foundation, Inc., 59 Temple Place, 
    Suite 330, Boston, MA 02111-1307, USA or visit their web page on the 
    internet at http://www.fsf.org/licenses/lgpl.html.
*/
#include "Main.h"
#include "Pass.h"
#include <string.h>
#include <assert.h>
#include "GrSegment.h"
#include "Code.h"
#include "Rule.h"
#include "XmlTraceLog.h"

using vm::Code;
using vm::Machine;
using namespace org::sil::graphite::v2;

Pass::Pass()
 :
    m_silf(0),
    m_cols(0),
    m_startStates(0),
    m_sTable(0),
    m_states(0),
    m_ruleMap(0),
    m_rules(0)
{
}

Pass::~Pass()
{
    free(m_cols);
    free(m_startStates);
    free(m_sTable);
    free(m_states);
    free(m_ruleMap);
    free(m_rules);
}

bool Pass::readPass(void *pPass, size_t lPass)
{
    const byte *                p = reinterpret_cast<const byte *>(pPass),
               * const pass_start = p,
	       * const pass_end   = p + lPass;
    size_t numRanges;

    // Read in basic values
    const byte flags = *p++;
    m_iMaxLoop             = *p++;
    const byte nMaxContext = *p++;
    p += sizeof(byte);     // skip maxBackup
    m_numRules             = read16(p);
    p += sizeof(uint16);   // not sure why we would want this
//    const byte * const pcCode = pass_start + read32(p),
//               * const rcCode = pass_start + read32(p),
//               * const aCode  = pass_start + read32(p);
    p += sizeof(uint32)*3;
    p += sizeof(uint32);
    m_sRows       = read16(p);
    m_sTransition = read16(p);
    m_sSuccess    = read16(p);
    m_sColumns    = read16(p);
    numRanges = read16(p);
    p += sizeof(uint16)   // skip searchRange
      +  sizeof(uint16)   // skip entrySelector
      +  sizeof(uint16);  // skip rangeShift
#ifndef DISABLE_TRACING
    XmlTraceLog::get().addAttribute(AttrFlags,          flags);
    XmlTraceLog::get().addAttribute(AttrMaxRuleLoop,    m_iMaxLoop);
    XmlTraceLog::get().addAttribute(AttrNumRules,       m_numRules);
    XmlTraceLog::get().addAttribute(AttrNumRows,        m_sRows);
    XmlTraceLog::get().addAttribute(AttrNumTransition,  m_sTransition);
    XmlTraceLog::get().addAttribute(AttrNumSuccess,     m_sSuccess);
    XmlTraceLog::get().addAttribute(AttrNumColumns,     m_sColumns);
    XmlTraceLog::get().addAttribute(AttrNumRanges,      numRanges);
#endif
    assert(p - pass_start == 40);
    // Perform some sanity checks.
    if (   m_sTransition > m_sRows 
	|| m_sSuccess > m_sRows) 
      return false;

    m_numGlyphs = swap16(*(uint16 *)(p + numRanges * 6 - 4)) + 1;
    // Caculate the start of vairous arrays.
    const uint16 * const ranges = reinterpret_cast<const uint16 *>(p); 
    p += numRanges*sizeof(uint16)*3;
    const uint16 * const o_rule_map = reinterpret_cast<const uint16 *>(p); 
    p += (m_sSuccess + 1)*sizeof(uint16);
    
    // More sanity checks
    if (p > pass_end) return false;
    const size_t numEntries = swap16(o_rule_map[m_sSuccess]);
    const uint16 * const   rule_map = reinterpret_cast<const uint16 *>(p);
    p += numEntries*sizeof(uint16);
    
    if (p > pass_end) return false;
    m_minPreCtxt = *p++;
    m_maxPreCtxt = *p++;
    const int16 * const start_states = reinterpret_cast<const int16 *>(p);
    p += (m_maxPreCtxt - m_minPreCtxt + 1)*sizeof(int16);
    const uint16 * const sort_keys = reinterpret_cast<const uint16 *>(p);
    p += m_numRules*sizeof(uint16);
    const byte * const precontext = p; p += m_numRules;
    p += sizeof(byte);     // skip reserved byte

    if (p > pass_end) return false;
    const size_t pass_constraint_len = read16(p);
    const uint16 * const o_constraint = reinterpret_cast<const uint16 *>(p);
    p += (m_numRules + 1)*sizeof(uint16);
    const uint16 * const o_actions = reinterpret_cast<const uint16 *>(p);
    p += (m_numRules + 1)*sizeof(uint16);
    const int16 * const states = reinterpret_cast<const int16 *>(p);
    p += m_sTransition*m_sColumns*sizeof(int16);
    p += sizeof(byte);          // skip reserved byte
//    if (p != pcCode || p >= pass_end) return false;
    const byte * const pcCode = p;
    p += pass_constraint_len; 
//    if (p != rcCode || p >= pass_end) return false;
    const byte * const rcCode = p;
    p += swap16(o_constraint[m_numRules]); 
//    if (p != aCode || p >= pass_end) return false;
    const byte * const aCode = p;
    if (size_t(rcCode - pcCode) != pass_constraint_len) return false;

    // Load the pass constraint if there is one.
    if (pass_constraint_len)
    {
      m_cPConstraint = vm::Code(true, pcCode, pcCode + pass_constraint_len);
      if (!m_cPConstraint) return false;
    }
    bool success = true;
    if (!readRanges(ranges, numRanges)) return false;
    if (!readRules(rule_map, numEntries,  precontext, sort_keys, 
			 o_constraint, rcCode, o_actions, aCode)) return false;
    return readStates(start_states, states, o_rule_map);
}


bool Pass::readRules(const uint16 * rule_map, const size_t num_entries, 
		     const byte *precontext, const uint16 * sort_key,
		     const uint16 * o_constraint, const byte *rc_data, 
		     const uint16 * o_action,     const byte * ac_data)
{
  // Load rules
  Rule    * r;
  const byte * ac_data_end = ac_data + o_action[m_numRules];
  const byte * rc_data_end = rc_data + o_constraint[m_numRules];
  const byte * ac_begin = ac_data + swap16(*o_action++), 
             * rc_begin = rc_data + swap16(*o_constraint++),
	     * ac_end = 0, * rc_end = 0;
  m_rules = r = gralloc<Rule>(m_numRules);
  for (size_t n = m_numRules; n; --n, ++r, ++o_action, ac_begin = ac_end, ++o_constraint, rc_begin = rc_end)
  {
    r->preContext = *precontext++;
    ac_end = *o_action     ? ac_data + swap16(*o_action) : ac_begin;
    rc_end = *o_constraint ? rc_data + swap16(*o_constraint) : rc_begin;
    
    if (ac_begin > ac_end || ac_begin >= ac_data_end || ac_end > ac_data_end
	|| rc_begin > rc_end || rc_begin >= rc_data_end || rc_end > rc_data_end)
      return false;
    r->action     = new vm::Code(false, ac_begin, ac_end);
    r->constraint = new vm::Code(true,  rc_begin, rc_end);

    if (!r->action || !r->constraint)  
      return false;
    logRule(r, sort_key);
  }
  
  // Load the rule entries map
  RuleEntry * re;
  m_ruleMap = re = gralloc<RuleEntry>(num_entries);
  for (size_t n = num_entries; n; --n, ++re)
  {
    const ptrdiff_t rn = swap16(*rule_map++);
    if (rn >= m_numRules)  return false;
    re->rule = m_rules + rn;
    re->sort = swap16(sort_key[rn]);
  }
  
  return true;
}


bool Pass::readStates(const int16 * starts, const int16 *states, const uint16 * o_rule_map)
{
  m_startStates = gralloc<State *>(m_maxPreCtxt - m_minPreCtxt + 1);
  m_states      = gralloc<State>(m_sRows);
  m_sTable      = gralloc<State *>(m_sTransition * m_sColumns);

  // load start states
  for (State * * s = m_startStates, 
             * * const s_end = s + m_maxPreCtxt - m_minPreCtxt + 1; s != s_end; ++s)
  {
    *s = m_states + swap16(*starts++);
    if (*s < m_states || *s >= m_states + m_sRows) return true;
  }
  
  // load state transition table.
  for (State * *               t = m_sTable, 
             * * const t_end = t + m_sTransition*m_sColumns; t != t_end; ++t)
  {
    *t = m_states + swap16(*states++);
    if (*t < m_states || *t >= m_states + m_sRows) return false;
  }

  State * s = m_states,
        * const transitions_end = m_states + m_sTransition,
	* const success_begin   = m_states + m_sRows - m_sSuccess;
  const RuleEntry * rule_map_end = m_ruleMap + swap16(o_rule_map[m_sSuccess]);
  for (size_t n = m_sRows; n; --n, ++s)
  {
      s->transitions = s < transitions_end ? m_sTable + (s-m_states)*m_sColumns : 0;
      RuleEntry * const begin = s < success_begin ? 0 : m_ruleMap + swap16(*o_rule_map++),
                * const end   = s < success_begin ? 0 : m_ruleMap + swap16(*o_rule_map);
      
      if (begin >= rule_map_end || end > rule_map_end || begin > end)
	return false;
      
      s->rules = begin;
      s->rules_end = end;
  }

  logStates();
  return true;
}


void Pass::logRule(const Rule * r, const uint16 * sort_key) const
{
#ifndef DISABLE_TRACING
    if (!XmlTraceLog::get().active()) return;
    
    const size_t lid = r - m_rules;
    if (r->constraint)
    {
      XmlTraceLog::get().openElement(ElementConstraint);
      XmlTraceLog::get().addAttribute(AttrIndex, lid);
      XmlTraceLog::get().closeElement(ElementConstraint);      
    }
    else
    {
      XmlTraceLog::get().openElement(ElementRule);
      XmlTraceLog::get().addAttribute(AttrIndex, lid);
      XmlTraceLog::get().addAttribute(AttrSortKey, swap16(sort_key[lid]));
      XmlTraceLog::get().addAttribute(AttrPrecontext, r->preContext);
      XmlTraceLog::get().closeElement(ElementRule);
    }
#endif
}

void Pass::logStates() const
{
#ifndef DISABLE_TRACING
  if (XmlTraceLog::get().active())
  {
    for (size_t i = 0; i != (m_maxPreCtxt - m_minPreCtxt + 1); ++i)
    {
      XmlTraceLog::get().openElement(ElementStartState);
      XmlTraceLog::get().addAttribute(AttrContextLen, i + m_minPreCtxt);
      XmlTraceLog::get().addAttribute(AttrState, m_startStates[i]);
      XmlTraceLog::get().closeElement(ElementStartState);
    }
      
    for (uint16 i = 0; i != m_sSuccess; ++i)
    {
      XmlTraceLog::get().openElement(ElementRuleMap);
      XmlTraceLog::get().addAttribute(AttrSuccessId, i);
      for (const RuleEntry *j = m_states[i].rules, *const j_end = m_states[i].rules_end; j != j_end; ++j)
      {
        XmlTraceLog::get().openElement(ElementRule);
        XmlTraceLog::get().addAttribute(AttrRuleId, size_t(j->rule - m_rules));
        XmlTraceLog::get().closeElement(ElementRule);
      }
      XmlTraceLog::get().closeElement(ElementRuleMap);
    }

    XmlTraceLog::get().openElement(ElementStateTransitions);
    for (size_t iRow = 0; iRow < m_sTransition; iRow++)
    {
      XmlTraceLog::get().openElement(ElementRow);
      XmlTraceLog::get().addAttribute(AttrIndex, iRow);
      XmlTraceLog::get().writeElementArray(ElementData, AttrValue, 
	  m_sTable + iRow * m_sColumns, m_sColumns);
      XmlTraceLog::get().closeElement(ElementRow);
    }
    XmlTraceLog::get().closeElement(ElementStateTransitions);
  }
#endif
}

bool Pass::readRanges(const uint16 *ranges, size_t num_ranges)
{
  m_cols = gralloc<uint16>(m_numGlyphs);
  std::fill_n(m_cols, m_numGlyphs, uint16(-1));
  for (size_t n = num_ranges; n; --n)
  {
      const uint16 first = swap16(*ranges++),
		   last  = swap16(*ranges++),
		   col   = swap16(*ranges++);

      if (first > last || last >= m_numGlyphs || col >= m_sColumns) 
	return false;

#ifndef DISABLE_TRACING
      if (XmlTraceLog::get().active())
      {
	  XmlTraceLog::get().openElement(ElementRange);
	  XmlTraceLog::get().addAttribute(AttrFirstId, first);
	  XmlTraceLog::get().addAttribute(AttrLastId, last);
	  XmlTraceLog::get().addAttribute(AttrColId, col);
	  XmlTraceLog::get().closeElement(ElementRange);
      }
#endif
      std::fill(m_cols + first, m_cols + last + 1, col);
  }
  return true;
}

void Pass::runGraphite(GrSegment *seg, const GrFace *face, FiniteStateMachine & fsm) const
{
    if (!testPassConstraint(seg)) return;
    
    // advance may be negative, so it is dangerous to use unsigned for i
    int loopCount = m_iMaxLoop;
    int maxIndex = 0, currCount = 0;
    for (Slot *s = seg->first(); s; )
    {
        int count = 0;
        s = findNDoRule(seg, s, count, face, fsm);
        currCount += count;
        if (currCount <= maxIndex)
        {
            if (--loopCount < 0)
            {
                while (++currCount <= maxIndex && s) s = s->next();
                loopCount = m_iMaxLoop;
                maxIndex = currCount + 1;
            }
        }
        else
        {
            loopCount = m_iMaxLoop;
            maxIndex = currCount + 1;
        }
    }
}

Slot *Pass::findNDoRule(GrSegment *seg, Slot *iSlot, int &count, const GrFace *face, FiniteStateMachine & fsm) const
{
    Slot *startSlot = iSlot;
    int iCtxt = 0;
    int lcount = count;
    for (iCtxt = 0; iCtxt < m_maxPreCtxt && iSlot->prev(); iCtxt++, iSlot = iSlot->prev()) {}
    if (iCtxt < m_minPreCtxt)
    {
        count = 1;
        return startSlot->next();
    }

    fsm.rules.clear();
    const State * state = m_startStates[m_maxPreCtxt - iCtxt];
    while (true)
    {
        if (!iSlot)
        {
            iSlot = seg->last();
            break;
        }
        const uint16 gid = iSlot->gid();
        if (gid >= m_numGlyphs) break;
        fsm.slotMap[lcount] = iSlot;
        uint16 iCol = m_cols[gid];
        ++lcount;
#ifdef ENABLE_DEEP_TRACING
        if (state >= m_sTransition)
        {
            XmlTraceLog::get().error("Invalid state %d", state);
        }
        if (iCol >= m_sColumns && iCol != 65535)
        {
            XmlTraceLog::get().error("Invalid column %d ID %d for slot %d",
                iCol, gid, iSlot);
        }
#endif
        if (iCol == 0xffffU) break;
        state = state->transitions[iCol];
        if (state->is_success())
          fsm.rules.accumulate_rules(*state, lcount - iCtxt);
        if (state == m_states || !state->is_transition()) break;
        iSlot = iSlot->next();
    }
    fsm.slotMap[lcount] = iSlot ? iSlot->next() : iSlot;
    
    count = lcount;
    for (const RuleEntry *r = fsm.rules.begin(); r != fsm.rules.end(); ++r)
    {
#ifdef ENABLE_DEEP_TRACING
      if (XmlTraceLog::get().active())
      {
	XmlTraceLog::get().openElement(ElementTestRule);
	XmlTraceLog::get().addAttribute(AttrNum, r->rule - m_rules);
	XmlTraceLog::get().addAttribute(AttrIndex, count);
      }
#endif
      if (testConstraint(*r, startSlot, iCtxt, seg, lcount, fsm.slotMap))
      {
#ifdef ENABLE_DEEP_TRACING
	  if (XmlTraceLog::get().active())
	  {
		XmlTraceLog::get().closeElement(ElementTestRule);
		XmlTraceLog::get().openElement(ElementDoRule);
		XmlTraceLog::get().addAttribute(AttrNum, r->rule - m_rules);
		XmlTraceLog::get().addAttribute(AttrIndex, count);
	  }
#endif
	  Slot *res = doAction(r->rule->action, startSlot, count, iCtxt, seg, fsm.slotMap);
#ifdef ENABLE_DEEP_TRACING
	  if (XmlTraceLog::get().active())
	  {
  //	      XmlTraceLog::get().addAttribute(AttrResult, res);
	  XmlTraceLog::get().closeElement(ElementDoRule);
	  }
#endif
//            if (res == -1)
//                return m_ruleSorts[rulenum];
//            else
	  return res;
      }
#ifdef ENABLE_DEEP_TRACING
      else
	  XmlTraceLog::get().closeElement(ElementTestRule);
#endif
    }
    count = 1;
    return startSlot->next();
}


inline 
bool Pass::testPassConstraint(GrSegment *seg) const
{
    if (!m_cPConstraint) return true;
    
    assert(m_cPConstraint.constraint());
    
    Slot *first = seg->first();
    Machine::status_t status = Machine::finished;
    Machine m;
    int temp = 0;
    const uint32 ret = m_cPConstraint.run(m, *seg, first, temp, temp, 1, &first, status);
    return ret && status != Machine::finished;
}

int Pass::testConstraint(const RuleEntry &re, Slot *iSlot, int nCtxt, GrSegment *seg, int nMap, Slot **map) const
{
  const Rule &r = *re.rule;
  uint32 ret = 1;
  
  if (!*r.constraint) return 1;
  
  assert(r.constraint->constraint());
    
  Machine::status_t status = Machine::finished;
  Machine m;
  for (int i = r.preContext; i && iSlot->prev(); --i, iSlot = iSlot->prev());

  for (int i = -r.preContext; i != re.length; ++i, iSlot = iSlot->next())
  {
      int temp = i + nCtxt;
      ret = r.constraint->run(m, *seg, iSlot, temp, nCtxt, nMap, map, status);
      if (!ret) return 0;
      if (status != Machine::finished) return 1;
  }
  
  return status == Machine::finished ? ret : 1;
}

Slot *Pass::doAction(const Code *codeptr, Slot *iSlot, int &count, int nPre, GrSegment *seg, Slot **map) const
{
    if (!*codeptr)
        return iSlot->next();
    
    assert(!codeptr->constraint());
    
    Machine::status_t status;
    Machine m;
    int nMap = count;
    count = nPre;
    int oldNumGlyphs = seg->length();
    int32 ret = codeptr->run(m, *seg, iSlot, count, nPre, nMap, map, status);
    count += seg->length() - oldNumGlyphs;
    
    for (int i = 0; i < nMap; ++i)
    {
        if (map[i]->isCopied() || map[i]->isDeleted())
            seg->freeSlot(map[i]);
    }
    if (ret < 0)
    {
        if (!iSlot)
        {
            iSlot = seg->last();
            ++ret;
            --count;
        }
        while (++ret <= 0)
        {
            iSlot = iSlot->prev();
            --count;
        }
    }
    else if (ret > 0)
    {
        if (!iSlot)
        {
            iSlot = seg->first();
            --ret;
            ++count;
        }
        while (--ret >= 0)
        {
            iSlot = iSlot->next();
            ++count;
        }
    }
    count -= nPre;
    return iSlot;
}

