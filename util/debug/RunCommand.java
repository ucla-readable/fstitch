import java.util.*;

import command.*;

public class RunCommand implements Command
{
	public String getName()
	{
		return "run";
	}
	
	public String getHelp()
	{
		return "Apply all opcodes to system state.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			System.out.print("Replaying log... ");
			dbg.replayAll();
			System.out.println(dbg.getApplied() + " opcodes OK!");
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
