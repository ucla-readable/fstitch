import java.io.DataInput;
import java.io.IOException;

public class InfoBdescNumber extends Opcode
{
	private final int block, number;
	
	public InfoBdescNumber(int block, int number)
	{
		this.block = block;
		this.number = number;
	}
	
	public void applyTo(SystemState state)
	{
		state.setBdesc(block, number);
	}
	
	public boolean isSkippable()
	{
		return true;
	}
	
	public String toString()
	{
		return "KDB_INFO_BDESC_NUMBER: block = " + SystemState.hex(block) + ", number = " + number;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_INFO_BDESC_NUMBER, "KDB_INFO_BDESC_NUMBER", InfoBdescNumber.class);
		factory.addParameter("block", 4);
		factory.addParameter("number", 4);
		return factory;
	}
}
