import java.io.DataInput;
import java.io.IOException;

public class BdescDestroy extends Opcode
{
	private final int block, ddesc;
	
	public BdescDestroy(int block, int ddesc)
	{
		this.block = block;
		this.ddesc = ddesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public String toString()
	{
		return "KDB_BDESC_DESTROY: block = " + SystemState.hex(block) + ", ddesc = " + SystemState.hex(ddesc);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_DESTROY, "KDB_BDESC_DESTROY", BdescDestroy.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		return factory;
	}
}
