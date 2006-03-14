//import java.util.*;

import command.*;

public class ResetCommand implements Command
{
	public String getName()
	{
		return "reset";
	}
	
	public String getHelp()
	{
		return "Reset system state to 0 opcodes.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
			dbg.resetState();
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
