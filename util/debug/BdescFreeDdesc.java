import java.io.DataInput;
import java.io.IOException;

public class BdescFreeDdesc extends Opcode
{
	private final int block, ddesc;
	
	public BdescFreeDdesc(int block, int ddesc)
	{
		this.block = block;
		this.ddesc = ddesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_FREE_DDESC, "KDB_BDESC_FREE_DDESC", BdescFreeDdesc.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		return factory;
	}
}
