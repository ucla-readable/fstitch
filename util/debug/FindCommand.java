import java.util.*;

import command.*;

public class FindCommand implements Command
{
	public String getName()
	{
		return "find";
	}
	
	public String getHelp()
	{
		return "Find max or min change descriptor count, optionally in an opcode range.";
	}
	
	private static int findMax(Debugger dbg, int start, int stop)
	{
		int applied = dbg.getApplied();
		int max, maxOpcode = start;
		SystemState state;
		
		dbg.resetState();
		dbg.replay(start);
		
		state = dbg.getState();
		max = state.getChdescCount();
		
		stop -= start;
		for(int i = 0; i <= stop; i++)
		{
			if(state.getChdescCount() > max)
			{
				max = state.getChdescCount();
				maxOpcode = dbg.getApplied();
			}
			dbg.replay(1);
		}
		
		dbg.resetState();
		dbg.replay(applied);
		
		return maxOpcode;
	}
	
	private static int findMin(Debugger dbg, int start, int stop)
	{
		int applied = dbg.getApplied();
		int min, minOpcode = start;
		SystemState state;
		
		dbg.resetState();
		dbg.replay(start);
		
		state = dbg.getState();
		min = state.getChdescCount();
		
		stop -= start;
		for(int i = 0; i <= stop; i++)
		{
			if(state.getChdescCount() < min)
			{
				min = state.getChdescCount();
				minOpcode = dbg.getApplied();
			}
			dbg.replay(1);
		}
		
		dbg.resetState();
		dbg.replay(applied);
		
		return minOpcode;
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			if(args.length == 0 || !("max".equals(args[0]) || "min".equals(args[0])))
				System.out.println("Need \"max\" or \"min\" to find.");
			else if(args.length == 1 || args.length >= 3)
			{
				int opcode, start = 0, stop = dbg.getOpcodeCount();
				String range = "";
				if(args.length >= 3)
				{
					start = Integer.parseInt(args[1]);
					stop = Integer.parseInt(args[2]);
					range = "in range ";
				}
				if("max".equals(args[0]))
					opcode = findMax(dbg, start, stop);
				else
					opcode = findMin(dbg, start, stop);
				System.out.println("The " + args[0] + "imum change descriptor count " + range + "first occurs at opcode #" + opcode);
			}
			else
				System.out.println("Need a valid opcode range.");
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
