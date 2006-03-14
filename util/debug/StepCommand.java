//import java.util.*;

import command.*;

public class StepCommand implements Command
{
	public String getName()
	{
		return "step";
	}
	
	public String getHelp()
	{
		return "Step system state by a specified number of opcodes, or 1 by default.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			try {
				int count = 1;
				if(args.length != 0)
					count = Integer.parseInt(args[0]);
				System.out.print("Replaying log... ");
				if(count < 0)
				{
					count += dbg.getApplied();
					dbg.resetState();
				}
				if(dbg.replay(count))
					System.out.println(dbg.getApplied() + " opcodes OK!");
				else
					System.out.println(dbg.getApplied() + " opcodes OK, no change.");
			}
			catch(NumberFormatException e)
			{
				System.out.println("Invalid number.");
			}
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
